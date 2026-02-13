#include "search_controller.h"
#include "service_manager.h"
#include "core/ipc/service_base.h"
#include "core/ipc/supervisor.h"
#include "core/shared/logging.h"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <chrono>

namespace bs {

namespace {

bool envFlagEnabledInternal(const char* key, bool fallback = false)
{
    const QString value = qEnvironmentVariable(key).trimmed().toLower();
    if (value.isEmpty()) {
        return fallback;
    }
    return value == QLatin1String("1")
        || value == QLatin1String("true")
        || value == QLatin1String("yes")
        || value == QLatin1String("on");
}

QString normalizedBundleId(QString value)
{
    value = value.trimmed();
    return value.isEmpty() ? QString() : value.toLower();
}

QString metadataDigest(const QByteArray& seed)
{
    if (seed.isEmpty()) {
        return {};
    }
    return QString::fromUtf8(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

} // namespace

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

void SearchController::setServiceManager(ServiceManager* serviceManager)
{
    if (m_serviceManager == serviceManager) {
        return;
    }

    if (m_serviceManager) {
        disconnect(m_serviceManager, nullptr, this, nullptr);
    }

    m_serviceManager = serviceManager;
    if (m_serviceManager) {
        connect(m_serviceManager, &ServiceManager::healthSnapshotUpdated,
                this, &SearchController::onHealthSnapshotUpdated);
        m_lastHealthSnapshot = m_serviceManager->latestHealthSnapshot();
        m_lastHealthSnapshotTimeMs = QDateTime::currentMSecsSinceEpoch();
    }
}

void SearchController::onHealthSnapshotUpdated(const QJsonObject& snapshot)
{
    const QVariantMap map = snapshot.toVariantMap();
    if (map.isEmpty()) {
        return;
    }
    m_lastHealthSnapshot = map;
    m_lastHealthSnapshotTimeMs = QDateTime::currentMSecsSinceEpoch();
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

void SearchController::recordBehaviorEvent(const QJsonObject& event)
{
    QJsonObject payload = event;
    const QString existingEventId = payload.value(QStringLiteral("eventId")).toString().trimmed();
    if (existingEventId.isEmpty()) {
        payload[QStringLiteral("eventId")] =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!payload.contains(QStringLiteral("timestamp"))) {
        payload[QStringLiteral("timestamp")] =
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
    }
    const QString existingSource = payload.value(QStringLiteral("source")).toString().trimmed();
    if (existingSource.isEmpty()) {
        payload[QStringLiteral("source")] = QStringLiteral("betterspotlight");
    }
    const QString source = payload.value(QStringLiteral("source"))
                               .toString()
                               .trimmed()
                               .toLower();
    const QString eventType = payload.value(QStringLiteral("eventType"))
                                  .toString()
                                  .trimmed()
                                  .toLower();
    const QString eventId = payload.value(QStringLiteral("eventId")).toString().trimmed();

    const QString bundleId = normalizedBundleId(
        payload.value(QStringLiteral("appBundleId")).toString());
    if (!bundleId.isEmpty()) {
        m_lastFrontmostAppBundleId = bundleId;
        payload[QStringLiteral("appBundleId")] = bundleId;
    } else if (source == QLatin1String("betterspotlight")
               && !m_lastFrontmostAppBundleId.isEmpty()) {
        payload[QStringLiteral("appBundleId")] = m_lastFrontmostAppBundleId;
    }

    QString activityDigest = payload.value(QStringLiteral("activityDigest")).toString().trimmed();
    if (activityDigest.isEmpty()) {
        QByteArray seed = eventType.toUtf8();
        seed += '|';
        seed += bundleId.toUtf8();
        seed += '|';
        seed += eventId.toUtf8();
        seed += '|';
        seed += QString::number(payload.value(QStringLiteral("timestamp"))
                                    .toVariant()
                                    .toLongLong())
                    .toUtf8();
        activityDigest = metadataDigest(seed).left(32);
        if (!activityDigest.isEmpty()) {
            payload[QStringLiteral("activityDigest")] = activityDigest;
        }
    }

    if (source != QLatin1String("betterspotlight")) {
        if (!eventId.isEmpty()) {
            m_lastSystemEventId = eventId;
        }
        if (!activityDigest.isEmpty()) {
            m_lastSystemActivityDigest = activityDigest.left(32);
        }
        if (eventType == QLatin1String("app_activated")
            && !bundleId.isEmpty()) {
            m_lastFrontmostAppBundleId = bundleId;
        }
    }

    SocketClient* client = ensureQueryClient(150);
    if (!client || !client->isConnected()) {
        return;
    }
    client->sendNotification(QStringLiteral("record_behavior_event"), payload);
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
    SocketClient* client = ensureQueryClient(250);
    if (client && client->isConnected()) {
        QJsonObject params;
        QVariantMap item = m_results.at(resultIndex).toMap();
        params[QStringLiteral("itemId")] = item.value(QStringLiteral("itemId")).toLongLong();
        params[QStringLiteral("action")] = QStringLiteral("open");
        params[QStringLiteral("query")] = m_query;
        params[QStringLiteral("position")] = resultIndex;
        client->sendNotification(QStringLiteral("recordFeedback"), params);

        QJsonObject interactionParams;
        interactionParams[QStringLiteral("query")] = m_query;
        interactionParams[QStringLiteral("selectedItemId")] =
            item.value(QStringLiteral("itemId")).toLongLong();
        interactionParams[QStringLiteral("selectedPath")] = path;
        interactionParams[QStringLiteral("matchType")] =
            item.value(QStringLiteral("matchType")).toString();
        interactionParams[QStringLiteral("resultPosition")] = resultIndex + 1;
        if (!m_lastFrontmostAppBundleId.isEmpty()) {
            interactionParams[QStringLiteral("frontmostApp")] = m_lastFrontmostAppBundleId;
            interactionParams[QStringLiteral("appBundleId")] = m_lastFrontmostAppBundleId;
        }
        if (!m_lastContextEventId.isEmpty()) {
            interactionParams[QStringLiteral("contextEventId")] = m_lastContextEventId;
        }
        if (!m_lastActivityDigest.isEmpty()) {
            interactionParams[QStringLiteral("activityDigest")] = m_lastActivityDigest.left(32);
        }
        client->sendNotification(QStringLiteral("record_interaction"), interactionParams);

        QJsonObject behaviorParams;
        behaviorParams[QStringLiteral("eventId")] =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        behaviorParams[QStringLiteral("source")] = QStringLiteral("betterspotlight");
        behaviorParams[QStringLiteral("eventType")] = QStringLiteral("result_open");
        behaviorParams[QStringLiteral("timestamp")] =
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        behaviorParams[QStringLiteral("query")] = m_query;
        behaviorParams[QStringLiteral("itemId")] =
            item.value(QStringLiteral("itemId")).toLongLong();
        behaviorParams[QStringLiteral("itemPath")] = path;
        if (!m_lastFrontmostAppBundleId.isEmpty()) {
            behaviorParams[QStringLiteral("appBundleId")] = m_lastFrontmostAppBundleId;
        }
        if (!m_lastContextEventId.isEmpty()) {
            behaviorParams[QStringLiteral("contextEventId")] = m_lastContextEventId;
        }
        if (!m_lastActivityDigest.isEmpty()) {
            behaviorParams[QStringLiteral("activityDigest")] = m_lastActivityDigest.left(32);
        }
        recordBehaviorEvent(behaviorParams);
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

QVariantMap SearchController::requestAnswerSnippet(int index)
{
    QVariantMap responseSummary;
    responseSummary[QStringLiteral("ok")] = false;

    const int resultIndex = resultIndexForRow(index);
    if (resultIndex < 0 || resultIndex >= m_results.size()) {
        responseSummary[QStringLiteral("reason")] = QStringLiteral("invalid_index");
        return responseSummary;
    }

    SocketClient* client = ensureQueryClient(250);
    if (!client || !client->isConnected()) {
        responseSummary[QStringLiteral("reason")] = QStringLiteral("query_service_unavailable");
        return responseSummary;
    }

    QVariantMap item = m_results.at(resultIndex).toMap();
    const qint64 itemId = item.value(QStringLiteral("itemId")).toLongLong();
    const QString path = item.value(QStringLiteral("path")).toString();
    const QString trimmedQuery = m_query.trimmed();
    if (trimmedQuery.isEmpty() || (itemId <= 0 && path.isEmpty())) {
        responseSummary[QStringLiteral("reason")] = QStringLiteral("missing_input");
        return responseSummary;
    }

    item[QStringLiteral("answerStatus")] = QStringLiteral("loading");
    item[QStringLiteral("answerSnippet")] = QString();
    m_results[resultIndex] = item;
    rebuildResultRows();
    emit resultsChanged();
    emit resultRowsChanged();
    emit selectedIndexChanged();

    QJsonObject params;
    params[QStringLiteral("query")] = trimmedQuery;
    params[QStringLiteral("itemId")] = itemId;
    params[QStringLiteral("path")] = path;
    params[QStringLiteral("timeoutMs")] = 350;
    params[QStringLiteral("maxChars")] = 240;

    const auto response = client->sendRequest(QStringLiteral("getAnswerSnippet"), params, 1200);
    if (!response.has_value()) {
        item[QStringLiteral("answerStatus")] = QStringLiteral("unavailable");
        item[QStringLiteral("answerSnippet")] = QString();
        item[QStringLiteral("answerReason")] = QStringLiteral("request_failed");
        m_results[resultIndex] = item;
        rebuildResultRows();
        emit resultsChanged();
        emit resultRowsChanged();
        emit selectedIndexChanged();
        responseSummary[QStringLiteral("reason")] = QStringLiteral("request_failed");
        return responseSummary;
    }

    const QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        const QString reason = response->value(QStringLiteral("error"))
                                   .toObject()
                                   .value(QStringLiteral("message"))
                                   .toString(QStringLiteral("request_error"));
        item[QStringLiteral("answerStatus")] = QStringLiteral("error");
        item[QStringLiteral("answerSnippet")] = QString();
        item[QStringLiteral("answerReason")] = reason;
        m_results[resultIndex] = item;
        rebuildResultRows();
        emit resultsChanged();
        emit resultRowsChanged();
        emit selectedIndexChanged();
        responseSummary[QStringLiteral("reason")] = reason;
        return responseSummary;
    }

    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    const bool available = result.value(QStringLiteral("available")).toBool(false);
    const QString answer = result.value(QStringLiteral("answer")).toString();
    const QString reason = result.value(QStringLiteral("reason")).toString();
    item[QStringLiteral("answerSnippet")] = answer;
    item[QStringLiteral("answerReason")] = reason;
    item[QStringLiteral("answerConfidence")] = result.value(QStringLiteral("confidence")).toDouble();
    item[QStringLiteral("answerSource")] = result.value(QStringLiteral("source")).toString();
    item[QStringLiteral("answerStatus")] = available ? QStringLiteral("ready")
                                                     : QStringLiteral("no_answer");

    m_results[resultIndex] = item;
    rebuildResultRows();
    emit resultsChanged();
    emit resultRowsChanged();
    emit selectedIndexChanged();

    responseSummary[QStringLiteral("ok")] = available;
    responseSummary[QStringLiteral("reason")] = reason;
    responseSummary[QStringLiteral("answer")] = answer;
    responseSummary[QStringLiteral("confidence")] =
        result.value(QStringLiteral("confidence")).toDouble();
    return responseSummary;
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

bool SearchController::envFlagEnabled(const char* key, bool fallback)
{
    return envFlagEnabledInternal(key, fallback);
}

SocketClient* SearchController::ensureQueryClient(int timeoutMs)
{
    if (!m_queryClient) {
        m_queryClient = std::make_unique<SocketClient>(this);
    }
    if (!m_queryClient->isConnected()) {
        m_queryClient->connectToServer(ServiceBase::socketPath(QStringLiteral("query")),
                                       timeoutMs);
    }
    return m_queryClient.get();
}

SocketClient* SearchController::ensureIndexerClient(int timeoutMs)
{
    if (!m_indexerClient) {
        m_indexerClient = std::make_unique<SocketClient>(this);
    }
    if (!m_indexerClient->isConnected()) {
        m_indexerClient->connectToServer(ServiceBase::socketPath(QStringLiteral("indexer")),
                                         timeoutMs);
    }
    return m_indexerClient.get();
}

QVariantMap SearchController::getHealthSync()
{
    const auto staleSnapshot = [&](const QString& reason) -> QVariantMap {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!m_lastHealthSnapshot.isEmpty()) {
            QVariantMap stale = m_lastHealthSnapshot;
            stale[QStringLiteral("snapshotState")] = QStringLiteral("stale");
            stale[QStringLiteral("overallStatus")] = QStringLiteral("stale");
            stale[QStringLiteral("healthStatusReason")] = reason;
            stale[QStringLiteral("staleReason")] = reason;
            stale[QStringLiteral("stalenessMs")] =
                std::max<qint64>(0, now - m_lastHealthSnapshotTimeMs);
            return stale;
        }

        QVariantMap unavailable;
        unavailable[QStringLiteral("overallStatus")] = QStringLiteral("unavailable");
        unavailable[QStringLiteral("snapshotState")] = QStringLiteral("unavailable");
        unavailable[QStringLiteral("healthStatusReason")] = reason;
        unavailable[QStringLiteral("stalenessMs")] = static_cast<qint64>(0);
        unavailable[QStringLiteral("instanceId")] =
            qEnvironmentVariable("BETTERSPOTLIGHT_INSTANCE_ID");
        return unavailable;
    };

    const QString mode = qEnvironmentVariable("BETTERSPOTLIGHT_HEALTH_SOURCE_MODE")
                             .trimmed()
                             .toLower();
    const bool actorPreferred = mode != QLatin1String("legacy");
    const bool actorOnly = mode == QLatin1String("aggregator_primary");

    if (actorPreferred && m_serviceManager) {
        QVariantMap latest = m_serviceManager->latestHealthSnapshot();
        if (latest.isEmpty()) {
            latest = m_lastHealthSnapshot;
        }
        if (!latest.isEmpty()) {
            m_lastHealthSnapshot = latest;
            m_lastHealthSnapshotTimeMs = QDateTime::currentMSecsSinceEpoch();
            return m_lastHealthSnapshot;
        }
        if (actorOnly) {
            return staleSnapshot(QStringLiteral("health_aggregator_unavailable"));
        }
    }

    SocketClient* client = ensureQueryClient(250);
    if (!client || !client->isConnected()) {
        return staleSnapshot(QStringLiteral("query_unavailable"));
    }

    auto response = client->sendRequest(QStringLiteral("getHealthV2"), {}, 1200);
    if (!response) {
        response = client->sendRequest(QStringLiteral("getHealth"), {}, 1200);
    }
    if (!response || response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
        return staleSnapshot(QStringLiteral("health_rpc_error"));
    }

    QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QJsonObject indexHealth = result.value(QStringLiteral("indexHealth")).toObject();
    if (indexHealth.isEmpty()) {
        indexHealth = result;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    indexHealth[QStringLiteral("snapshotState")] = QStringLiteral("fresh");
    indexHealth[QStringLiteral("stalenessMs")] = static_cast<qint64>(0);
    if (!indexHealth.contains(QStringLiteral("snapshotTimeMs"))) {
        indexHealth[QStringLiteral("snapshotTimeMs")] = now;
    }
    if (!indexHealth.contains(QStringLiteral("instanceId"))) {
        indexHealth[QStringLiteral("instanceId")] =
            qEnvironmentVariable("BETTERSPOTLIGHT_INSTANCE_ID");
    }
    if (!indexHealth.contains(QStringLiteral("overallStatus"))) {
        indexHealth[QStringLiteral("overallStatus")] = QStringLiteral("unavailable");
    }

    const QVariantMap healthMap = indexHealth.toVariantMap();
    if (healthMap.isEmpty()) {
        return staleSnapshot(QStringLiteral("empty_health_payload"));
    }

    m_lastHealthSnapshot = healthMap;
    m_lastHealthSnapshotTimeMs = now;
    return m_lastHealthSnapshot;
}

void SearchController::executeSearch()
{
    QString trimmedQuery = m_query.trimmed();
    if (trimmedQuery.isEmpty()) {
        return;
    }

    SocketClient* client = ensureQueryClient(300);
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "SearchController: query service not connected");
        return;
    }

    SocketClient* indexerClient = ensureIndexerClient(200);
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
    m_lastContextEventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray digestSeed = trimmedQuery.toUtf8();
    if (!m_lastFrontmostAppBundleId.isEmpty()) {
        digestSeed += '|';
        digestSeed += m_lastFrontmostAppBundleId.toUtf8();
    }
    if (!m_lastSystemActivityDigest.isEmpty()) {
        digestSeed += '|';
        digestSeed += m_lastSystemActivityDigest.toUtf8();
    }
    if (!m_lastSystemEventId.isEmpty()) {
        digestSeed += '|';
        digestSeed += m_lastSystemEventId.toUtf8();
    }
    m_lastActivityDigest = metadataDigest(digestSeed);
    context[QStringLiteral("contextEventId")] = m_lastContextEventId;
    context[QStringLiteral("contextFeatureVersion")] = 1;
    context[QStringLiteral("activityDigest")] = m_lastActivityDigest.left(32);
    if (!m_lastFrontmostAppBundleId.isEmpty()) {
        context[QStringLiteral("frontmostAppBundleId")] = m_lastFrontmostAppBundleId;
    }
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

    if (client && client->isConnected()) {
        QJsonObject behaviorParams;
        behaviorParams[QStringLiteral("eventId")] = m_lastContextEventId;
        behaviorParams[QStringLiteral("source")] = QStringLiteral("betterspotlight");
        behaviorParams[QStringLiteral("eventType")] = QStringLiteral("query_submitted");
        behaviorParams[QStringLiteral("timestamp")] =
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        behaviorParams[QStringLiteral("activityDigest")] = m_lastActivityDigest.left(32);
        behaviorParams[QStringLiteral("contextEventId")] = m_lastContextEventId;
        behaviorParams[QStringLiteral("query")] = trimmedQuery;
        if (!m_lastFrontmostAppBundleId.isEmpty()) {
            behaviorParams[QStringLiteral("appBundleId")] = m_lastFrontmostAppBundleId;
        }
        QJsonObject inputMeta;
        inputMeta[QStringLiteral("keyEventCount")] = trimmedQuery.size();
        inputMeta[QStringLiteral("shortcutCount")] = 0;
        inputMeta[QStringLiteral("scrollCount")] = 0;
        inputMeta[QStringLiteral("metadataOnly")] = true;
        behaviorParams[QStringLiteral("inputMeta")] = inputMeta;
        recordBehaviorEvent(behaviorParams);
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
        item[QStringLiteral("answerSnippet")] = QString();
        item[QStringLiteral("answerStatus")] = QStringLiteral("idle");
        item[QStringLiteral("answerReason")] = QString();
        item[QStringLiteral("answerConfidence")] = 0.0;
        item[QStringLiteral("answerSource")] = QString();

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
