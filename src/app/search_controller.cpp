#include "search_controller.h"
#include "core/ipc/supervisor.h"
#include "core/shared/logging.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QUrl>

namespace bs {

SearchController::SearchController(QObject* parent)
    : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(kDebounceMs);
    connect(&m_debounceTimer, &QTimer::timeout,
            this, &SearchController::executeSearch);
}

SearchController::~SearchController() = default;

void SearchController::setSupervisor(Supervisor* supervisor)
{
    m_supervisor = supervisor;
}

QString SearchController::query() const
{
    return m_query;
}

void SearchController::setQuery(const QString& query)
{
    if (m_query == query) {
        return;
    }

    m_query = query;
    emit queryChanged();

    if (m_query.trimmed().isEmpty()) {
        // Clear results immediately for empty queries
        m_results.clear();
        m_selectedIndex = -1;
        emit resultsChanged();
        emit selectedIndexChanged();
        m_debounceTimer.stop();
        return;
    }

    // Restart the debounce timer
    m_debounceTimer.start();
}

QVariantList SearchController::results() const
{
    return m_results;
}

bool SearchController::isSearching() const
{
    return m_isSearching;
}

int SearchController::selectedIndex() const
{
    return m_selectedIndex;
}

void SearchController::setSelectedIndex(int index)
{
    if (index < -1) {
        index = -1;
    }
    if (index >= m_results.size()) {
        index = m_results.size() - 1;
    }

    if (m_selectedIndex == index) {
        return;
    }

    m_selectedIndex = index;
    emit selectedIndexChanged();
}

void SearchController::openResult(int index)
{
    QString path = pathForResult(index);
    if (path.isEmpty()) {
        return;
    }

    LOG_INFO(bsCore, "SearchController: opening '%s'", qPrintable(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));

    // Record feedback via IPC (fire and forget)
    if (m_supervisor) {
        SocketClient* client = m_supervisor->clientFor(QStringLiteral("query"));
        if (client && client->isConnected()) {
            QJsonObject params;
            QVariantMap item = m_results.at(index).toMap();
            params[QStringLiteral("itemId")] = item.value(QStringLiteral("itemId")).toLongLong();
            params[QStringLiteral("action")] = QStringLiteral("open");
            params[QStringLiteral("query")] = m_query;
            params[QStringLiteral("position")] = index;
            client->sendNotification(QStringLiteral("recordFeedback"), params);
        }
    }
}

void SearchController::revealInFinder(int index)
{
    QString path = pathForResult(index);
    if (path.isEmpty()) {
        return;
    }

    LOG_INFO(bsCore, "SearchController: revealing '%s' in Finder", qPrintable(path));
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
}

void SearchController::copyPath(int index)
{
    QString path = pathForResult(index);
    if (path.isEmpty()) {
        return;
    }

    LOG_INFO(bsCore, "SearchController: copying path '%s'", qPrintable(path));
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard) {
        clipboard->setText(path);
    }
}

void SearchController::clearResults()
{
    m_query.clear();
    m_results.clear();
    m_selectedIndex = -1;
    m_debounceTimer.stop();

    emit queryChanged();
    emit resultsChanged();
    emit selectedIndexChanged();
}

QVariantMap SearchController::getHealthSync()
{
    QVariantMap emptyResult;

    if (!m_supervisor) {
        LOG_WARN(bsCore, "SearchController: no supervisor set");
        return emptyResult;
    }

    SocketClient* client = m_supervisor->clientFor(QStringLiteral("query"));
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "SearchController: query service not connected");
        return emptyResult;
    }

    auto response = client->sendRequest(QStringLiteral("getHealth"), {}, kSearchTimeoutMs);
    if (!response) {
        LOG_WARN(bsCore, "SearchController: getHealth request failed");
        return emptyResult;
    }

    // Check for error
    QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        LOG_WARN(bsCore, "SearchController: getHealth returned error");
        return emptyResult;
    }

    QJsonObject result = response->value(QStringLiteral("result")).toObject();

    // The query service nests health stats under "indexHealth".
    // Flatten it so QML can access keys like healthData["totalIndexedItems"] directly.
    QJsonObject indexHealth = result.value(QStringLiteral("indexHealth")).toObject();
    return indexHealth.toVariantMap();
}

void SearchController::executeSearch()
{
    QString trimmedQuery = m_query.trimmed();
    if (trimmedQuery.isEmpty()) {
        return;
    }

    if (!m_supervisor) {
        LOG_WARN(bsCore, "SearchController: no supervisor set, cannot search");
        return;
    }

    SocketClient* client = m_supervisor->clientFor(QStringLiteral("query"));
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "SearchController: query service not connected");
        return;
    }

    m_isSearching = true;
    emit isSearchingChanged();

    LOG_DEBUG(bsCore, "SearchController: searching for '%s'", qPrintable(trimmedQuery));

    QJsonObject params;
    params[QStringLiteral("query")] = trimmedQuery;
    params[QStringLiteral("limit")] = 20;

    auto response = client->sendRequest(QStringLiteral("search"), params, kSearchTimeoutMs);

    m_isSearching = false;
    emit isSearchingChanged();

    if (!response) {
        LOG_WARN(bsCore, "SearchController: search request failed (timeout or disconnected)");
        return;
    }

    // Check if the query changed while we were waiting (stale response)
    if (m_query.trimmed() != trimmedQuery) {
        LOG_DEBUG(bsCore, "SearchController: discarding stale search results");
        return;
    }

    parseSearchResponse(*response);
}

void SearchController::parseSearchResponse(const QJsonObject& response)
{
    // Check for error
    QString type = response.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        QString msg = response.value(QStringLiteral("error")).toObject()
                          .value(QStringLiteral("message")).toString();
        LOG_WARN(bsCore, "SearchController: search error: %s", qPrintable(msg));
        return;
    }

    QJsonObject result = response.value(QStringLiteral("result")).toObject();
    QJsonArray resultsArray = result.value(QStringLiteral("results")).toArray();

    QVariantList newResults;
    newResults.reserve(resultsArray.size());

    for (const auto& val : resultsArray) {
        QJsonObject obj = val.toObject();

        QVariantMap item;
        item[QStringLiteral("itemId")] = obj.value(QStringLiteral("itemId")).toInteger();
        item[QStringLiteral("path")] = obj.value(QStringLiteral("path")).toString();
        item[QStringLiteral("name")] = obj.value(QStringLiteral("name")).toString();
        item[QStringLiteral("kind")] = obj.value(QStringLiteral("kind")).toString();
        item[QStringLiteral("matchType")] = obj.value(QStringLiteral("matchType")).toString();
        item[QStringLiteral("score")] = obj.value(QStringLiteral("score")).toDouble();
        item[QStringLiteral("snippet")] = obj.value(QStringLiteral("snippet")).toString();
        item[QStringLiteral("fileSize")] = obj.value(QStringLiteral("fileSize")).toInteger();
        item[QStringLiteral("modifiedAt")] = obj.value(QStringLiteral("modifiedAt")).toString();

        // Compute parent path for display
        QString path = item.value(QStringLiteral("path")).toString();
        int lastSlash = path.lastIndexOf(QLatin1Char('/'));
        item[QStringLiteral("parentPath")] = (lastSlash > 0) ? path.left(lastSlash) : path;

        newResults.append(item);
    }

    m_results = std::move(newResults);
    m_selectedIndex = m_results.isEmpty() ? -1 : 0;

    emit resultsChanged();
    emit selectedIndexChanged();

    LOG_DEBUG(bsCore, "SearchController: got %d results", static_cast<int>(m_results.size()));
}

QString SearchController::pathForResult(int index) const
{
    if (index < 0 || index >= m_results.size()) {
        return {};
    }

    QVariantMap item = m_results.at(index).toMap();
    return item.value(QStringLiteral("path")).toString();
}

} // namespace bs
