#include "search_controller.h"
#include "core/ipc/supervisor.h"
#include "core/shared/logging.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>

namespace bs {

SearchController::SearchController(QObject* parent)
    : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(kDebounceMs);
    connect(&m_debounceTimer, &QTimer::timeout,
            this, &SearchController::executeSearch);

    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        connect(clipboard, &QClipboard::changed,
                this, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard) {
                handleClipboardChanged();
            }
        });
    }
}

SearchController::~SearchController() = default;

void SearchController::setSupervisor(Supervisor* supervisor)
{
    m_supervisor = supervisor;
}

void SearchController::setClipboardSignalsEnabled(bool enabled)
{
    if (m_clipboardSignalsEnabled == enabled) {
        return;
    }
    m_clipboardSignalsEnabled = enabled;

    if (!m_clipboardSignalsEnabled) {
        clearClipboardSignals();
        return;
    }

    handleClipboardChanged();
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
        m_resultRows.clear();
        m_selectedIndex = -1;
        emit resultsChanged();
        emit resultRowsChanged();
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

QVariantList SearchController::resultRows() const
{
    return m_resultRows;
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
    if (m_resultRows.isEmpty()) {
        index = -1;
    } else {
        if (index < -1) {
            index = -1;
        }
        if (index >= m_resultRows.size()) {
            index = m_resultRows.size() - 1;
        }

        if (index >= 0 && resultIndexForRow(index) < 0) {
            int forward = nextSelectableRow(index, +1);
            if (forward >= 0) {
                index = forward;
            } else {
                index = nextSelectableRow(index, -1);
            }
        }
    }

    if (m_selectedIndex == index) {
        return;
    }

    m_selectedIndex = index;
    emit selectedIndexChanged();
}

void SearchController::openResult(int index)
{
    const int resultIndex = resultIndexForRow(index);
    QString path = pathForResult(resultIndex);
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
            QVariantMap item = m_results.at(resultIndex).toMap();
            params[QStringLiteral("itemId")] = item.value(QStringLiteral("itemId")).toLongLong();
            params[QStringLiteral("action")] = QStringLiteral("open");
            params[QStringLiteral("query")] = m_query;
            params[QStringLiteral("position")] = resultIndex;
            client->sendNotification(QStringLiteral("recordFeedback"), params);
        }
    }
}

void SearchController::revealInFinder(int index)
{
    QString path = pathForResult(resultIndexForRow(index));
    if (path.isEmpty()) {
        return;
    }

    LOG_INFO(bsCore, "SearchController: revealing '%s' in Finder", qPrintable(path));
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
}

void SearchController::copyPath(int index)
{
    QString path = pathForResult(resultIndexForRow(index));
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
    m_resultRows.clear();
    m_selectedIndex = -1;
    m_debounceTimer.stop();

    emit queryChanged();
    emit resultsChanged();
    emit resultRowsChanged();
    emit selectedIndexChanged();
}

void SearchController::moveSelection(int delta)
{
    if (delta == 0 || m_resultRows.isEmpty()) {
        return;
    }

    if (m_selectedIndex < 0) {
        setSelectedIndex(delta > 0 ? firstSelectableRow() : nextSelectableRow(m_resultRows.size(), -1));
        return;
    }

    setSelectedIndex(nextSelectableRow(m_selectedIndex, delta > 0 ? 1 : -1));
}

QVariantMap SearchController::getHealthSync()
{
    QVariantMap emptyResult;
    const auto cachedOrEmpty = [&]() -> QVariantMap {
        if (!m_lastHealthSnapshot.isEmpty()) {
            return m_lastHealthSnapshot;
        }
        return emptyResult;
    };

    if (!m_supervisor) {
        LOG_WARN(bsCore, "SearchController: no supervisor set");
        return cachedOrEmpty();
    }

    SocketClient* client = m_supervisor->clientFor(QStringLiteral("query"));
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "SearchController: query service not connected");
        return cachedOrEmpty();
    }

    auto response = client->sendRequest(QStringLiteral("getHealth"), {}, kSearchTimeoutMs);
    if (!response) {
        LOG_WARN(bsCore, "SearchController: getHealth request failed");
        return cachedOrEmpty();
    }

    // Check for error
    QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        LOG_WARN(bsCore, "SearchController: getHealth returned error");
        return cachedOrEmpty();
    }

    QJsonObject result = response->value(QStringLiteral("result")).toObject();

    // Prefer extended endpoint when available.
    auto detailResponse = client->sendRequest(QStringLiteral("getHealthDetails"), {}, kSearchTimeoutMs);
    if (detailResponse && detailResponse->value(QStringLiteral("type")).toString() != QLatin1String("error")) {
        result = detailResponse->value(QStringLiteral("result")).toObject();
    }

    // The query service nests health stats under "indexHealth".
    // Flatten it so QML can access keys like healthData["totalIndexedItems"] directly.
    QJsonObject indexHealth = result.value(QStringLiteral("indexHealth")).toObject();
    const QJsonObject details = result.value(QStringLiteral("details")).toObject();
    if (!details.isEmpty()) {
        indexHealth[QStringLiteral("detailedFailures")] = details.value(QStringLiteral("failures")).toArray();
        indexHealth[QStringLiteral("criticalFailureRows")] =
            details.value(QStringLiteral("criticalFailureRows")).toInt();
        indexHealth[QStringLiteral("expectedGapFailureRows")] =
            details.value(QStringLiteral("expectedGapFailureRows")).toInt();
        indexHealth[QStringLiteral("processStats")] = details.value(QStringLiteral("processStats")).toObject();
        indexHealth[QStringLiteral("queryStats")] = details.value(QStringLiteral("queryStats")).toObject();
        indexHealth[QStringLiteral("bsignoreDetails")] = details.value(QStringLiteral("bsignore")).toObject();
    }

    // Merge queue/runtime stats from the indexer service.
    SocketClient* indexerClient = m_supervisor->clientFor(QStringLiteral("indexer"));
    if (indexerClient && indexerClient->isConnected()) {
        auto queueResponse = indexerClient->sendRequest(QStringLiteral("getQueueStatus"), {},
                                                        kSearchTimeoutMs);
        if (queueResponse
            && queueResponse->value(QStringLiteral("type")).toString() != QLatin1String("error")) {
            const QJsonObject queueResult = queueResponse->value(QStringLiteral("result")).toObject();
            const int pending = queueResult.value(QStringLiteral("pending")).toInt();
            const int processing = queueResult.value(QStringLiteral("processing")).toInt();
            const int dropped = queueResult.value(QStringLiteral("dropped")).toInt();
            const bool paused = queueResult.value(QStringLiteral("paused")).toBool();

            indexHealth[QStringLiteral("queuePending")] = pending;
            indexHealth[QStringLiteral("queueInProgress")] = processing;
            indexHealth[QStringLiteral("queueDropped")] = dropped;
            indexHealth[QStringLiteral("queueSource")] = QStringLiteral("indexer_rpc");
            indexHealth[QStringLiteral("queuePreparing")] =
                queueResult.value(QStringLiteral("preparing")).toInt();
            indexHealth[QStringLiteral("queueWriting")] =
                queueResult.value(QStringLiteral("writing")).toInt();
            indexHealth[QStringLiteral("queueCoalesced")] =
                queueResult.value(QStringLiteral("coalesced")).toInt();
            indexHealth[QStringLiteral("queueStaleDropped")] =
                queueResult.value(QStringLiteral("staleDropped")).toInt();
            indexHealth[QStringLiteral("queuePrepWorkers")] =
                queueResult.value(QStringLiteral("prepWorkers")).toInt();
            indexHealth[QStringLiteral("queueWriterBatchDepth")] =
                queueResult.value(QStringLiteral("writerBatchDepth")).toInt();

            const QJsonArray roots = queueResult.value(QStringLiteral("roots")).toArray();
            if (!roots.isEmpty()) {
                QJsonArray rootStatus;
                const QString status = paused
                                           ? QStringLiteral("active")
                                           : (processing > 0 ? QStringLiteral("scanning")
                                                             : QStringLiteral("active"));
                for (const QJsonValue& root : roots) {
                    QJsonObject rootEntry;
                    rootEntry[QStringLiteral("path")] = root.toString();
                    rootEntry[QStringLiteral("status")] = status;
                    rootStatus.append(rootEntry);
                }
                indexHealth[QStringLiteral("indexRoots")] = rootStatus;
            }

            if (indexHealth.value(QStringLiteral("healthStatusReason")).toString()
                == QLatin1String("indexer_unavailable")) {
                const bool rebuilding =
                    indexHealth.value(QStringLiteral("overallStatus")).toString()
                        == QLatin1String("rebuilding")
                    || indexHealth.value(QStringLiteral("vectorRebuildStatus")).toString()
                        == QLatin1String("running")
                    || indexHealth.value(QStringLiteral("totalIndexedItems")).toInteger() == 0;
                const int criticalFailures =
                    indexHealth.value(QStringLiteral("criticalFailures")).toInt();

                if (rebuilding) {
                    indexHealth[QStringLiteral("overallStatus")] = QStringLiteral("rebuilding");
                    indexHealth[QStringLiteral("healthStatusReason")] = QStringLiteral("rebuilding");
                } else if (criticalFailures > 0) {
                    indexHealth[QStringLiteral("overallStatus")] = QStringLiteral("degraded");
                    indexHealth[QStringLiteral("healthStatusReason")] =
                        QStringLiteral("degraded_critical_failures");
                } else {
                    indexHealth[QStringLiteral("overallStatus")] = QStringLiteral("healthy");
                    indexHealth[QStringLiteral("healthStatusReason")] = QStringLiteral("healthy");
                }
            }
        }
    }

    if (!indexHealth.contains(QStringLiteral("contentCoveragePct"))) {
        const qint64 totalIndexed = indexHealth.value(QStringLiteral("totalIndexedItems")).toInteger();
        const qint64 withoutContent = indexHealth.value(QStringLiteral("itemsWithoutContent")).toInteger();
        if (totalIndexed > 0) {
            const double coverage = 100.0 * static_cast<double>(totalIndexed - withoutContent)
                                    / static_cast<double>(totalIndexed);
            indexHealth[QStringLiteral("contentCoveragePct")] = coverage;
        }
    }

    const QVariantMap healthMap = indexHealth.toVariantMap();
    if (!healthMap.isEmpty()) {
        m_lastHealthSnapshot = healthMap;
    }
    return cachedOrEmpty();
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

    SocketClient* indexerClient = m_supervisor->clientFor(QStringLiteral("indexer"));
    const auto setIndexerActive = [indexerClient](bool active) {
        if (!indexerClient || !indexerClient->isConnected()) {
            return;
        }
        QJsonObject params;
        params[QStringLiteral("active")] = active;
        // Best effort signal: keep timeout short to avoid impacting search UX.
        indexerClient->sendRequest(QStringLiteral("setUserActive"), params, 250);
    };

    m_isSearching = true;
    emit isSearchingChanged();
    setIndexerActive(true);

    LOG_DEBUG(bsCore, "SearchController: searching for '%s'", qPrintable(trimmedQuery));

    QJsonObject params;
    params[QStringLiteral("query")] = trimmedQuery;
    params[QStringLiteral("limit")] = 20;
    QJsonObject context;
    if (m_clipboardSignalsEnabled) {
        if (m_clipboardBasenameSignal.has_value()) {
            context[QStringLiteral("clipboardBasename")] = *m_clipboardBasenameSignal;
        }
        if (m_clipboardDirnameSignal.has_value()) {
            context[QStringLiteral("clipboardDirname")] = *m_clipboardDirnameSignal;
        }
        if (m_clipboardExtensionSignal.has_value()) {
            context[QStringLiteral("clipboardExtension")] = *m_clipboardExtensionSignal;
        }
    }
    if (!context.isEmpty()) {
        params[QStringLiteral("context")] = context;
    }

    auto response = client->sendRequest(QStringLiteral("search"), params, kSearchTimeoutMs);

    m_isSearching = false;
    emit isSearchingChanged();
    setIndexerActive(false);

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
        const QJsonObject metadata = obj.value(QStringLiteral("metadata")).toObject();

        QVariantMap item;
        item[QStringLiteral("itemId")] = obj.value(QStringLiteral("itemId")).toInteger();
        item[QStringLiteral("path")] = obj.value(QStringLiteral("path")).toString();
        item[QStringLiteral("name")] = obj.value(QStringLiteral("name")).toString();
        item[QStringLiteral("kind")] = obj.value(QStringLiteral("kind")).toString();
        item[QStringLiteral("matchType")] = obj.value(QStringLiteral("matchType")).toString();
        item[QStringLiteral("score")] = obj.value(QStringLiteral("score")).toDouble();
        item[QStringLiteral("snippet")] = obj.value(QStringLiteral("snippet")).toString();
        item[QStringLiteral("fileSize")] = metadata.value(QStringLiteral("fileSize")).toInteger();
        item[QStringLiteral("modifiedAt")] = metadata.value(QStringLiteral("modificationDate")).toString();
        item[QStringLiteral("frequency")] = metadata.value(QStringLiteral("frequency")).toInteger();
        item[QStringLiteral("contentAvailable")] =
            obj.value(QStringLiteral("contentAvailable")).toBool(true);
        item[QStringLiteral("availabilityStatus")] =
            obj.value(QStringLiteral("availabilityStatus")).toString(QStringLiteral("available"));

        // Compute parent path for display
        QString path = item.value(QStringLiteral("path")).toString();
        int lastSlash = path.lastIndexOf(QLatin1Char('/'));
        item[QStringLiteral("parentPath")] = (lastSlash > 0) ? path.left(lastSlash) : path;

        newResults.append(item);
    }

    m_results = std::move(newResults);
    rebuildResultRows();
    m_selectedIndex = firstSelectableRow();

    emit resultsChanged();
    emit resultRowsChanged();
    emit selectedIndexChanged();

    LOG_DEBUG(bsCore, "SearchController: got %d results", static_cast<int>(m_results.size()));
}

void SearchController::rebuildResultRows()
{
    QVariantList recentRows;
    QVariantList folderRows;
    QVariantList fileRows;

    for (int i = 0; i < m_results.size(); ++i) {
        const QVariantMap item = m_results.at(i).toMap();
        QVariantMap row;
        row[QStringLiteral("rowType")] = QStringLiteral("result");
        row[QStringLiteral("resultIndex")] = i;
        row[QStringLiteral("itemData")] = item;

        const QString kind = item.value(QStringLiteral("kind")).toString();
        const int frequency = item.value(QStringLiteral("frequency")).toInt();
        if (frequency > 0) {
            recentRows.append(row);
        } else if (kind == QLatin1String("directory")) {
            folderRows.append(row);
        } else {
            fileRows.append(row);
        }
    }

    QVariantList rows;
    const auto appendGroup = [&rows](const QString& title, const QVariantList& groupRows) {
        if (groupRows.isEmpty()) {
            return;
        }
        QVariantMap header;
        header[QStringLiteral("rowType")] = QStringLiteral("header");
        header[QStringLiteral("title")] = title;
        rows.append(header);
        for (const QVariant& row : groupRows) {
            rows.append(row);
        }
    };

    appendGroup(QStringLiteral("Recently Opened"), recentRows);
    appendGroup(QStringLiteral("Folders"), folderRows);
    appendGroup(QStringLiteral("Files"), fileRows);

    m_resultRows = std::move(rows);
}

int SearchController::resultIndexForRow(int rowIndex) const
{
    if (rowIndex < 0 || rowIndex >= m_resultRows.size()) {
        return -1;
    }

    const QVariantMap row = m_resultRows.at(rowIndex).toMap();
    if (row.value(QStringLiteral("rowType")).toString() != QLatin1String("result")) {
        return -1;
    }
    bool ok = false;
    const int resultIndex = row.value(QStringLiteral("resultIndex")).toInt(&ok);
    return ok ? resultIndex : -1;
}

int SearchController::firstSelectableRow() const
{
    return nextSelectableRow(-1, +1);
}

int SearchController::nextSelectableRow(int fromIndex, int delta) const
{
    if (m_resultRows.isEmpty() || delta == 0) {
        return -1;
    }

    const int step = delta > 0 ? 1 : -1;
    int idx = fromIndex + step;
    while (idx >= 0 && idx < m_resultRows.size()) {
        if (resultIndexForRow(idx) >= 0) {
            return idx;
        }
        idx += step;
    }
    return -1;
}

QString SearchController::pathForResult(int index) const
{
    if (index < 0 || index >= m_results.size()) {
        return {};
    }

    QVariantMap item = m_results.at(index).toMap();
    return item.value(QStringLiteral("path")).toString();
}

void SearchController::handleClipboardChanged()
{
    if (!m_clipboardSignalsEnabled) {
        clearClipboardSignals();
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        clearClipboardSignals();
        return;
    }
    updateClipboardSignalsFromText(clipboard->text(QClipboard::Clipboard));
}

void SearchController::clearClipboardSignals()
{
    m_clipboardBasenameSignal.reset();
    m_clipboardDirnameSignal.reset();
    m_clipboardExtensionSignal.reset();
}

void SearchController::updateClipboardSignalsFromText(const QString& text)
{
    clearClipboardSignals();
    if (!m_clipboardSignalsEnabled) {
        return;
    }

    QString candidate = text;
    const int newline = candidate.indexOf(QLatin1Char('\n'));
    if (newline >= 0) {
        candidate = candidate.left(newline);
    }
    candidate = candidate.trimmed();
    if (candidate.isEmpty() || candidate.size() > 2048) {
        return;
    }

    if (candidate.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        const QUrl url(candidate);
        if (!url.isLocalFile()) {
            return;
        }
        candidate = url.toLocalFile();
    }
    if (candidate.startsWith(QStringLiteral("~/"))) {
        candidate = QDir::home().filePath(candidate.mid(2));
    }

    const auto setSignalsFromFileInfo = [this](const QFileInfo& info) {
        const QString fileName = info.fileName().toLower();
        if (!fileName.isEmpty()) {
            m_clipboardBasenameSignal = fileName;
            const QString ext = info.suffix().toLower();
            if (!ext.isEmpty()) {
                m_clipboardExtensionSignal = ext;
            }
        }
        const QString parentName = info.dir().dirName().toLower();
        if (!parentName.isEmpty() && parentName != QLatin1String(".")) {
            m_clipboardDirnameSignal = parentName;
        }
    };

    if (candidate.contains(QLatin1Char('/')) || candidate.contains(QLatin1Char('\\'))
        || candidate.startsWith(QLatin1Char('.'))) {
        setSignalsFromFileInfo(QFileInfo(QDir::cleanPath(candidate)));
        return;
    }

    static const QRegularExpression filenamePattern(
        QStringLiteral(R"(\b([A-Za-z0-9._-]+\.[A-Za-z0-9]{1,10})\b)"));
    const QRegularExpressionMatch filenameMatch = filenamePattern.match(candidate);
    if (filenameMatch.hasMatch()) {
        const QString filename = filenameMatch.captured(1).toLower();
        if (!filename.isEmpty()) {
            m_clipboardBasenameSignal = filename;
            const QFileInfo info(filename);
            const QString ext = info.suffix().toLower();
            if (!ext.isEmpty()) {
                m_clipboardExtensionSignal = ext;
            }
        }
    }
}

} // namespace bs
