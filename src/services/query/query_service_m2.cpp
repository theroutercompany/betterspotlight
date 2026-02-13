#include "query_service.h"

#include "core/feedback/feedback_aggregator.h"
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/path_preferences.h"
#include "core/feedback/type_affinity.h"
#include "core/ipc/message.h"
#include "core/ipc/socket_client.h"
#include "core/shared/logging.h"
#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"
#include "core/learning/learning_engine.h"
#include "core/learning/behavior_types.h"

#include <sqlite3.h>

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QJsonArray>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace bs {

namespace {
constexpr int kVectorRebuildProgressPersistStride = 32;
constexpr qint64 kVectorRebuildProgressPersistIntervalMs = 1500;
constexpr int kVectorRebuildChunksPerItem = 3;
constexpr int kVectorRebuildMaxChunkChars = 8192;

bool envFlagEnabled(const QString& raw)
{
    const QString normalized = raw.trimmed().toLower();
    return normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on");
}

bool testFakeEmbeddingsEnabled()
{
    if (!qEnvironmentVariableIsSet("BS_TEST_FAKE_EMBEDDINGS")) {
        return false;
    }
    return envFlagEnabled(qEnvironmentVariable("BS_TEST_FAKE_EMBEDDINGS"));
}

bool testFakeFastEmbeddingsEnabled()
{
    if (!qEnvironmentVariableIsSet("BS_TEST_FAKE_FAST_EMBEDDINGS")) {
        return true;
    }
    return envFlagEnabled(qEnvironmentVariable("BS_TEST_FAKE_FAST_EMBEDDINGS"));
}

int testFakeEmbeddingDimensions(const char* key, int fallback)
{
    bool ok = false;
    const int parsed = qEnvironmentVariableIntValue(key, &ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(parsed, 8, 4096);
}

std::vector<float> makeDeterministicEmbedding(const QString& text, int dimensions, uint seed)
{
    std::vector<float> embedding(static_cast<size_t>(std::max(dimensions, 1)), 0.0f);
    uint state = qHash(text, seed);
    double normSquared = 0.0;
    for (int i = 0; i < dimensions; ++i) {
        state = state * 1664525u + 1013904223u;
        const float unit = static_cast<float>(static_cast<double>(state & 0xFFFFu) / 65535.0);
        const float value = (unit * 2.0f) - 1.0f;
        embedding[static_cast<size_t>(i)] = value;
        normSquared += static_cast<double>(value) * static_cast<double>(value);
    }

    const double norm = std::sqrt(normSquared);
    if (norm > 0.0) {
        for (float& value : embedding) {
            value = static_cast<float>(static_cast<double>(value) / norm);
        }
    }
    return embedding;
}
} // namespace

QJsonObject QueryService::handleRecordInteraction(uint64_t id, const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    QString query = params.value(QStringLiteral("query")).toString();
    if (query.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'query' parameter"));
    }

    int64_t selectedItemId = static_cast<int64_t>(
        params.value(QStringLiteral("selectedItemId")).toInteger());
    if (selectedItemId <= 0) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing or invalid 'selectedItemId'"));
    }

    InteractionTracker::Interaction interaction;
    interaction.query = query;
    interaction.selectedItemId = selectedItemId;
    interaction.selectedPath = params.value(QStringLiteral("selectedPath")).toString();
    interaction.matchType = params.value(QStringLiteral("matchType")).toString();
    interaction.resultPosition = params.value(QStringLiteral("resultPosition")).toInt(0);
    interaction.frontmostApp = params.value(QStringLiteral("frontmostApp")).toString();
    interaction.timestamp = QDateTime::currentDateTimeUtc();

    const bool ok = m_interactionTracker && m_interactionTracker->recordInteraction(interaction);
    if (!ok) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to record interaction"));
    }

    if (m_learningEngine) {
        m_learningEngine->noteUserActivity();
        m_learningEngine->recordPositiveInteraction(interaction.query,
                                                    interaction.selectedItemId,
                                                    interaction.selectedPath,
                                                    interaction.frontmostApp,
                                                    interaction.timestamp);
        QString idleReason;
        m_learningEngine->maybeRunIdleCycle(&idleReason);
    }

    if (m_pathPreferences) {
        m_pathPreferences->invalidateCache();
    }
    if (m_typeAffinity) {
        m_typeAffinity->invalidateCache();
    }

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
    if (m_learningEngine) {
        result[QStringLiteral("learningHealth")] = m_learningEngine->healthSnapshot();
    }
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetPathPreferences(uint64_t id, const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    int limit = params.value(QStringLiteral("limit")).toInt(50);
    if (limit < 1) {
        limit = 1;
    } else if (limit > 200) {
        limit = 200;
    }

    if (!m_pathPreferences) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("PathPreferences not initialized"));
    }

    const QVector<PathPreferences::DirPreference> dirs = m_pathPreferences->getTopDirectories(limit);

    QJsonArray dirsArray;
    for (const auto& dir : dirs) {
        QJsonObject obj;
        obj[QStringLiteral("directory")] = dir.directory;
        obj[QStringLiteral("selectionCount")] = dir.selectionCount;
        obj[QStringLiteral("boost")] = dir.boost;
        dirsArray.append(obj);
    }

    QJsonObject result;
    result[QStringLiteral("directories")] = dirsArray;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetFileTypeAffinity(uint64_t id)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_typeAffinity) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("TypeAffinity not initialized"));
    }

    const auto stats = m_typeAffinity->getAffinityStats();

    QJsonObject result;
    result[QStringLiteral("codeOpens")] = stats.codeOpens;
    result[QStringLiteral("documentOpens")] = stats.documentOpens;
    result[QStringLiteral("mediaOpens")] = stats.mediaOpens;
    result[QStringLiteral("otherOpens")] = stats.otherOpens;
    result[QStringLiteral("primaryAffinity")] = stats.primaryAffinity;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleRunAggregation(uint64_t id)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_feedbackAggregator) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("FeedbackAggregator not initialized"));
    }

    const bool aggOk = m_feedbackAggregator->runAggregation();
    const bool cleanupOk = m_feedbackAggregator->cleanup();

    if (m_pathPreferences) {
        m_pathPreferences->invalidateCache();
    }
    if (m_typeAffinity) {
        m_typeAffinity->invalidateCache();
    }

    QJsonObject result;
    result[QStringLiteral("aggregated")] = aggOk;
    result[QStringLiteral("cleanedUp")] = cleanupOk;
    result[QStringLiteral("lastAggregation")] =
        m_feedbackAggregator->lastAggregationTime().toString(Qt::ISODate);
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleExportInteractionData(uint64_t id, const QJsonObject& params)
{
    Q_UNUSED(params);

    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_interactionTracker) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("InteractionTracker not initialized"));
    }

    const QJsonArray data = m_interactionTracker->exportData();

    QJsonObject result;
    result[QStringLiteral("interactions")] = data;
    result[QStringLiteral("count")] = data.size();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleRecordBehaviorEvent(uint64_t id, const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }
    if (!m_learningEngine) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Learning engine not initialized"));
    }

    BehaviorEvent event;
    event.eventId = params.value(QStringLiteral("eventId")).toString();
    event.source = params.value(QStringLiteral("source")).toString(QStringLiteral("betterspotlight"));
    event.eventType = params.value(QStringLiteral("eventType")).toString(QStringLiteral("activity"));
    event.appBundleId = params.value(QStringLiteral("appBundleId")).toString();
    event.windowTitleHash = params.value(QStringLiteral("windowTitleHash")).toString();
    event.itemPath = params.value(QStringLiteral("itemPath")).toString();
    event.itemId = static_cast<int64_t>(params.value(QStringLiteral("itemId")).toInteger(0));
    event.browserHostHash = params.value(QStringLiteral("browserHostHash")).toString();
    event.attributionConfidence = std::clamp(
        params.value(QStringLiteral("attributionConfidence")).toDouble(0.0), 0.0, 1.0);
    event.contextEventId = params.value(QStringLiteral("contextEventId")).toString();
    event.activityDigest = params.value(QStringLiteral("activityDigest")).toString();

    const QJsonValue timestampValue = params.value(QStringLiteral("timestamp"));
    if (timestampValue.isDouble()) {
        const qint64 tsRaw = static_cast<qint64>(timestampValue.toDouble());
        if (tsRaw > 9999999999LL) {
            event.timestamp = QDateTime::fromMSecsSinceEpoch(tsRaw, QTimeZone::UTC);
        } else {
            event.timestamp = QDateTime::fromSecsSinceEpoch(tsRaw, QTimeZone::UTC);
        }
    } else if (timestampValue.isString()) {
        event.timestamp = QDateTime::fromString(timestampValue.toString(), Qt::ISODate);
        if (event.timestamp.isValid()) {
            event.timestamp = event.timestamp.toUTC();
        }
    } else {
        event.timestamp = QDateTime::currentDateTimeUtc();
    }

    const QJsonObject inputMeta = params.value(QStringLiteral("inputMeta")).toObject();
    event.inputMeta.keyEventCount = std::max(0, inputMeta.value(QStringLiteral("keyEventCount")).toInt(0));
    event.inputMeta.shortcutCount = std::max(0, inputMeta.value(QStringLiteral("shortcutCount")).toInt(0));
    event.inputMeta.scrollCount = std::max(0, inputMeta.value(QStringLiteral("scrollCount")).toInt(0));
    event.inputMeta.metadataOnly = inputMeta.value(QStringLiteral("metadataOnly")).toBool(true);

    const QJsonObject mouseMeta = params.value(QStringLiteral("mouseMeta")).toObject();
    event.mouseMeta.moveDistancePx = std::max(0.0, mouseMeta.value(QStringLiteral("moveDistancePx")).toDouble(0.0));
    event.mouseMeta.clickCount = std::max(0, mouseMeta.value(QStringLiteral("clickCount")).toInt(0));
    event.mouseMeta.dragCount = std::max(0, mouseMeta.value(QStringLiteral("dragCount")).toInt(0));

    const QJsonObject privacyFlags = params.value(QStringLiteral("privacyFlags")).toObject();
    event.privacyFlags.secureInput = privacyFlags.value(QStringLiteral("secureInput")).toBool(false);
    event.privacyFlags.privateContext = privacyFlags.value(QStringLiteral("privateContext")).toBool(false);
    event.privacyFlags.denylistedApp = privacyFlags.value(QStringLiteral("denylistedApp")).toBool(false);
    event.privacyFlags.redacted = privacyFlags.value(QStringLiteral("redacted")).toBool(false);

    QString error;
    const bool recorded = m_learningEngine->recordBehaviorEvent(event, &error);
    if (!recorded) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     error.isEmpty()
                                         ? QStringLiteral("Failed to record behavior event")
                                         : error);
    }

    bool attributedPositive = false;
    const QString query = params.value(QStringLiteral("query")).toString();
    if (!query.trimmed().isEmpty() && event.itemId > 0) {
        const QString eventTypeLower = event.eventType.trimmed().toLower();
        if (eventTypeLower == QLatin1String("open")
            || eventTypeLower == QLatin1String("select")
            || eventTypeLower == QLatin1String("activate")
            || eventTypeLower == QLatin1String("result_open")) {
            attributedPositive = m_learningEngine->recordPositiveInteraction(
                query, event.itemId, event.itemPath, event.appBundleId, event.timestamp);
        }
    }

    m_learningEngine->noteUserActivity();
    QString idleReason;
    const bool idleCycleTriggered = m_learningEngine->maybeRunIdleCycle(&idleReason);

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
    result[QStringLiteral("attributedPositive")] = attributedPositive;
    result[QStringLiteral("idleCycleTriggered")] = idleCycleTriggered;
    result[QStringLiteral("idleCycleReason")] = idleReason;
    result[QStringLiteral("learningHealth")] = m_learningEngine->healthSnapshot();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetLearningHealth(uint64_t id)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }
    if (!m_learningEngine) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Learning engine not initialized"));
    }

    QJsonObject result;
    result[QStringLiteral("learning")] = m_learningEngine->healthSnapshot();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleSetLearningConsent(uint64_t id, const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }
    if (!m_learningEngine) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Learning engine not initialized"));
    }

    const bool behaviorStreamEnabled =
        params.value(QStringLiteral("behaviorStreamEnabled")).toBool(false);
    const bool learningEnabled =
        params.value(QStringLiteral("learningEnabled")).toBool(false);
    const bool learningPauseOnUserInput =
        params.value(QStringLiteral("learningPauseOnUserInput")).toBool(true);

    QStringList denylistApps;
    const QJsonArray denylist = params.value(QStringLiteral("denylistApps")).toArray();
    denylistApps.reserve(denylist.size());
    for (const QJsonValue& value : denylist) {
        const QString app = value.toString().trimmed();
        if (!app.isEmpty() && !denylistApps.contains(app)) {
            denylistApps.push_back(app);
        }
    }

    QString error;
    const bool ok = m_learningEngine->setConsent(behaviorStreamEnabled,
                                                  learningEnabled,
                                                  learningPauseOnUserInput,
                                                  denylistApps,
                                                  &error);
    if (!ok) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     error.isEmpty()
                                         ? QStringLiteral("Failed to update learning consent")
                                         : error);
    }

    QJsonObject result;
    result[QStringLiteral("updated")] = true;
    result[QStringLiteral("learning")] = m_learningEngine->healthSnapshot();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleTriggerLearningCycle(uint64_t id, const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }
    if (!m_learningEngine) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Learning engine not initialized"));
    }

    const bool manual = params.value(QStringLiteral("manual")).toBool(true);
    QString reason;
    const bool promoted = m_learningEngine->triggerLearningCycle(manual, &reason);

    QJsonObject result;
    result[QStringLiteral("promoted")] = promoted;
    result[QStringLiteral("reason")] = reason;
    result[QStringLiteral("learning")] = m_learningEngine->healthSnapshot();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleRebuildVectorIndex(uint64_t id,
                                                   const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    const bool fakeEmbeddings = testFakeEmbeddingsEnabled();
    if ((!m_embeddingManager || !m_embeddingManager->isAvailable()) && !fakeEmbeddings) {
        return IpcMessage::makeError(id, IpcErrorCode::Unsupported,
                                     QStringLiteral("Embedding model is unavailable"));
    }

    if (!m_vectorStore) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Vector store is not initialized"));
    }

    if (m_dbPath.isEmpty() || !m_modelRegistry) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Vector rebuild is not configured"));
    }

    QStringList includePaths;
    if (params.contains(QStringLiteral("includePaths"))) {
        const QJsonArray includePathsArray = params.value(QStringLiteral("includePaths")).toArray();
        for (const QJsonValue& value : includePathsArray) {
            QString path = QDir::cleanPath(value.toString().trimmed());
            if (path.isEmpty()) {
                continue;
            }
            while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
            }
            if (!includePaths.contains(path)) {
                includePaths.append(path);
            }
        }
    }

    const QString requestedGeneration = params.value(QStringLiteral("targetGeneration"))
        .toString()
        .trimmed();
    const QString targetGeneration = requestedGeneration.isEmpty()
        ? (m_targetVectorGeneration.isEmpty() ? QStringLiteral("v2") : m_targetVectorGeneration)
        : requestedGeneration;
    m_targetVectorGeneration = targetGeneration;
    const QString targetIndexPath = vectorIndexPathForGeneration(targetGeneration);
    const QString targetMetaPath = vectorMetaPathForGeneration(targetGeneration);

    uint64_t runId = 0;
    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        if (m_vectorRebuildState.status == VectorRebuildState::Status::Running) {
            QJsonObject result;
            result[QStringLiteral("started")] = false;
            result[QStringLiteral("alreadyRunning")] = true;
            result[QStringLiteral("runId")] = static_cast<qint64>(m_vectorRebuildState.runId);
            result[QStringLiteral("status")] = vectorRebuildStatusToString(m_vectorRebuildState.status);
            return IpcMessage::makeResponse(id, result);
        }

        m_stopRebuildRequested.store(false);
        runId = m_vectorRebuildState.runId + 1;
        m_vectorRebuildState = VectorRebuildState{};
        m_vectorRebuildState.status = VectorRebuildState::Status::Running;
        m_vectorRebuildState.runId = runId;
        m_vectorRebuildState.startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        m_vectorRebuildState.scopeRoots = includePaths;
    }
    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        m_vectorMigrationState = QStringLiteral("building");
        m_vectorMigrationProgressPct = 0.0;
    }
    m_store->setSetting(QStringLiteral("targetVectorGeneration"), targetGeneration);
    m_store->setSetting(QStringLiteral("vectorMigrationState"), QStringLiteral("building"));
    m_store->setSetting(QStringLiteral("vectorMigrationProgressPct"), QStringLiteral("0"));

    joinVectorRebuildThread();
    m_vectorRebuildThread = std::thread(
        &QueryService::runVectorRebuildWorker, this, runId, m_dbPath, m_dataDir,
        m_modelRegistry->modelsDir(), targetIndexPath, targetMetaPath,
        targetGeneration,
        includePaths);

    QJsonObject result;
    result[QStringLiteral("started")] = true;
    result[QStringLiteral("alreadyRunning")] = false;
    result[QStringLiteral("runId")] = static_cast<qint64>(runId);
    result[QStringLiteral("targetGeneration")] = targetGeneration;
    result[QStringLiteral("status")] = QStringLiteral("running");
    return IpcMessage::makeResponse(id, result);
}

void QueryService::runVectorRebuildWorker(uint64_t runId,
                                          QString dbPath,
                                          QString dataDir,
                                          QString modelsDir,
                                          QString indexPath,
                                          QString metaPath,
                                          QString targetGeneration,
                                          QStringList includePaths)
{
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    bool inTransaction = false;

    auto updateFailedState = [&](const QString& error) {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        if (m_vectorRebuildState.runId != runId) {
            return;
        }
        m_vectorRebuildState.status = VectorRebuildState::Status::Failed;
        m_vectorRebuildState.finishedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        m_vectorRebuildState.lastError = error;
        m_vectorMigrationState = QStringLiteral("failed");
    };

    auto updateSucceededState = [&](int totalCandidates, int processed,
                                    int embedded, int skipped, int failed,
                                    const QString& lastError = QString()) {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        if (m_vectorRebuildState.runId != runId) {
            return;
        }
        m_vectorRebuildState.status = lastError.isEmpty()
                                          ? VectorRebuildState::Status::Succeeded
                                          : VectorRebuildState::Status::Failed;
        m_vectorRebuildState.totalCandidates = totalCandidates;
        m_vectorRebuildState.processed = processed;
        m_vectorRebuildState.embedded = embedded;
        m_vectorRebuildState.skipped = skipped;
        m_vectorRebuildState.failed = failed;
        m_vectorRebuildState.scopeCandidates = totalCandidates;
        m_vectorRebuildState.finishedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        m_vectorRebuildState.lastError = lastError;
        m_vectorMigrationState = lastError.isEmpty()
            ? QStringLiteral("cutover-complete")
            : QStringLiteral("failed");
        m_vectorMigrationProgressPct = lastError.isEmpty() ? 100.0 : m_vectorMigrationProgressPct;
    };

    auto closeResources = [&]() {
        if (stmt) {
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    };

    auto execSql = [&](const char* sql) -> bool {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            LOG_ERROR(bsIpc, "QueryService vector rebuild SQL failed: %s",
                      errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    };

    auto rollbackIfNeeded = [&]() {
        if (inTransaction) {
            execSql("ROLLBACK;");
            inTransaction = false;
        }
    };

    if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        updateFailedState(
            QStringLiteral("Failed to open rebuild database: %1")
                .arg(QString::fromUtf8(db ? sqlite3_errmsg(db) : "unknown")));
        closeResources();
        return;
    }
    sqlite3_busy_timeout(db, 5000);

    const auto readBoolSettingFromDb = [&](const QString& key, bool defaultValue) {
        static constexpr const char* kSql =
            "SELECT value FROM settings WHERE key = ?1 LIMIT 1";
        sqlite3_stmt* settingStmt = nullptr;
        if (sqlite3_prepare_v2(db, kSql, -1, &settingStmt, nullptr) != SQLITE_OK) {
            return defaultValue;
        }
        const QByteArray keyUtf8 = key.toUtf8();
        sqlite3_bind_text(settingStmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
        QString raw;
        if (sqlite3_step(settingStmt) == SQLITE_ROW) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(settingStmt, 0));
            raw = value ? QString::fromUtf8(value) : QString();
        }
        sqlite3_finalize(settingStmt);
        if (raw.isEmpty()) {
            return defaultValue;
        }
        return raw.compare(QStringLiteral("1"), Qt::CaseInsensitive) == 0
            || raw.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
            || raw.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
            || raw.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0;
    };

    const bool inferenceServiceEnabled =
        readBoolSettingFromDb(QStringLiteral("inferenceServiceEnabled"), true);
    const bool inferenceEmbedOffloadEnabled =
        readBoolSettingFromDb(QStringLiteral("inferenceEmbedOffloadEnabled"), true);
    const bool inferenceEmbedOffloadActive =
        inferenceServiceEnabled && inferenceEmbedOffloadEnabled;
    SocketClient inferenceClient;
    bool inferenceConnected = false;
    bool inferenceEmbedOffloadRunEnabled = false;
    bool inferenceFallbackLogged = false;
    if (inferenceEmbedOffloadActive) {
        const QString inferenceSocketPath = ServiceBase::socketPath(QStringLiteral("inference"));
        inferenceConnected = inferenceClient.connectToServer(inferenceSocketPath, 500);
        if (!inferenceConnected) {
            LOG_WARN(bsIpc,
                     "Vector rebuild run %llu: inference offload unavailable, falling back to local embeddings",
                     static_cast<unsigned long long>(runId));
        } else {
            inferenceEmbedOffloadRunEnabled = true;
        }
    }

    const bool fakeEmbeddings = testFakeEmbeddingsEnabled();
    const bool fakeFastEmbeddings = testFakeFastEmbeddingsEnabled();

    std::unique_ptr<ModelRegistry> workerRegistry;
    std::unique_ptr<EmbeddingManager> workerEmbeddingManager;
    std::unique_ptr<EmbeddingManager> workerFastEmbeddingManager;

    std::function<std::vector<std::vector<float>>(const std::vector<QString>&)> embedStrongBatchLocal;
    std::function<std::vector<float>(const QString&)> embedStrongSingleLocal;
    std::function<std::vector<std::vector<float>>(const std::vector<QString>&)> embedFastBatchLocal;
    std::function<std::vector<float>(const QString&)> embedFastSingleLocal;

    int embeddingDimensions = 0;
    int fastEmbeddingDimensions = 0;
    bool fastEmbeddingAvailable = false;
    QString embeddingModelId;
    QString embeddingProvider;
    QString fastModelId;
    QString fastProvider;
    QString fastGeneration = QStringLiteral("v3_fast");

    if (fakeEmbeddings) {
        embeddingDimensions = testFakeEmbeddingDimensions("BS_TEST_FAKE_EMBEDDING_DIMS", 24);
        embeddingModelId = QStringLiteral("test-fake-bi-encoder");
        embeddingProvider = QStringLiteral("test-fake");
        embedStrongBatchLocal =
            [embeddingDimensions](const std::vector<QString>& texts) {
                std::vector<std::vector<float>> out;
                out.reserve(texts.size());
                for (const QString& text : texts) {
                    out.push_back(makeDeterministicEmbedding(
                        text, embeddingDimensions, /*seed=*/0xB510A3u));
                }
                return out;
            };
        embedStrongSingleLocal =
            [embeddingDimensions](const QString& text) {
                return makeDeterministicEmbedding(
                    text, embeddingDimensions, /*seed=*/0xB510A3u);
            };

        fastEmbeddingAvailable = fakeFastEmbeddings;
        if (fastEmbeddingAvailable) {
            fastEmbeddingDimensions = testFakeEmbeddingDimensions(
                "BS_TEST_FAKE_FAST_EMBEDDING_DIMS", embeddingDimensions);
            fastGeneration = QStringLiteral("v3_fast_test");
            fastModelId = QStringLiteral("test-fake-bi-encoder-fast");
            fastProvider = QStringLiteral("test-fake");
            embedFastBatchLocal =
                [fastEmbeddingDimensions](const std::vector<QString>& texts) {
                    std::vector<std::vector<float>> out;
                    out.reserve(texts.size());
                    for (const QString& text : texts) {
                        out.push_back(makeDeterministicEmbedding(
                            text, fastEmbeddingDimensions, /*seed=*/0xF457A11u));
                    }
                    return out;
                };
            embedFastSingleLocal =
                [fastEmbeddingDimensions](const QString& text) {
                    return makeDeterministicEmbedding(
                        text, fastEmbeddingDimensions, /*seed=*/0xF457A11u);
                };
        }
    } else {
        workerRegistry = std::make_unique<ModelRegistry>(modelsDir);
        workerEmbeddingManager = std::make_unique<EmbeddingManager>(workerRegistry.get(), "bi-encoder");
        workerFastEmbeddingManager = std::make_unique<EmbeddingManager>(workerRegistry.get(),
                                                                         "bi-encoder-fast");
        if (!workerEmbeddingManager->initialize()) {
            updateFailedState(QStringLiteral("Embedding model is unavailable"));
            closeResources();
            return;
        }
        fastEmbeddingAvailable = workerFastEmbeddingManager->initialize();

        embeddingDimensions = std::max(workerEmbeddingManager->embeddingDimensions(), 1);
        embeddingModelId = workerEmbeddingManager->activeModelId();
        embeddingProvider = workerEmbeddingManager->providerName();
        embedStrongBatchLocal =
            [strong = workerEmbeddingManager.get()](const std::vector<QString>& texts) {
                return strong->embedBatch(texts);
            };
        embedStrongSingleLocal =
            [strong = workerEmbeddingManager.get()](const QString& text) {
                return strong->embed(text);
            };

        fastGeneration = workerFastEmbeddingManager->activeGenerationId().isEmpty()
            ? QStringLiteral("v3_fast")
            : workerFastEmbeddingManager->activeGenerationId();
        fastEmbeddingDimensions = std::max(workerFastEmbeddingManager->embeddingDimensions(), 1);
        fastModelId = workerFastEmbeddingManager->activeModelId();
        fastProvider = workerFastEmbeddingManager->providerName();
        embedFastBatchLocal =
            [fast = workerFastEmbeddingManager.get()](const std::vector<QString>& texts) {
                return fast->embedBatch(texts);
            };
        embedFastSingleLocal =
            [fast = workerFastEmbeddingManager.get()](const QString& text) {
                return fast->embed(text);
            };
    }

    const auto embedBatchViaInference =
        [&](const QString& role,
            const std::vector<QString>& texts,
            int microBatchSize) -> std::vector<std::vector<float>> {
        std::vector<std::vector<float>> embeddings;
        if (!inferenceConnected || texts.empty()) {
            return embeddings;
        }

        QJsonArray textArray;
        for (const QString& text : texts) {
            textArray.append(text);
        }

        QJsonObject params;
        params[QStringLiteral("role")] = role;
        params[QStringLiteral("texts")] = textArray;
        params[QStringLiteral("normalize")] = true;
        params[QStringLiteral("priority")] = QStringLiteral("rebuild");
        params[QStringLiteral("microBatchSize")] = std::max(1, microBatchSize);
        params[QStringLiteral("requestId")] = QStringLiteral("rebuild-%1-%2-%3")
            .arg(runId)
            .arg(role)
            .arg(QDateTime::currentMSecsSinceEpoch());
        params[QStringLiteral("deadlineMs")] =
            QDateTime::currentMSecsSinceEpoch() + 12000;

        auto response = inferenceClient.sendRequest(QStringLiteral("embed_passages"), params, 12000);
        if (!response.has_value()
            || response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
            return {};
        }

        const QJsonObject payload = response->value(QStringLiteral("result")).toObject();
        if (payload.value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
            return {};
        }

        const QJsonArray embeddingRows = payload.value(QStringLiteral("result"))
            .toObject()
            .value(QStringLiteral("embeddings"))
            .toArray();
        embeddings.reserve(static_cast<size_t>(embeddingRows.size()));
        for (const QJsonValue& rowValue : embeddingRows) {
            const QJsonArray rowArray = rowValue.toArray();
            std::vector<float> row;
            row.reserve(static_cast<size_t>(rowArray.size()));
            for (const QJsonValue& value : rowArray) {
                row.push_back(static_cast<float>(value.toDouble(0.0)));
            }
            embeddings.push_back(std::move(row));
        }
        return embeddings;
    };

    VectorIndex::IndexMetadata rebuiltMeta;
    rebuiltMeta.dimensions = embeddingDimensions;
    rebuiltMeta.modelId = embeddingModelId.toStdString();
    rebuiltMeta.generationId = targetGeneration.toStdString();
    rebuiltMeta.provider = embeddingProvider.toStdString();

    auto rebuiltIndex = std::make_unique<VectorIndex>(rebuiltMeta);
    if (!rebuiltIndex->create()) {
        updateFailedState(QStringLiteral("Failed to initialize vector index"));
        closeResources();
        return;
    }

    VectorIndex::IndexMetadata rebuiltFastMeta;
    rebuiltFastMeta.dimensions = fastEmbeddingDimensions;
    rebuiltFastMeta.modelId = fastModelId.toStdString();
    rebuiltFastMeta.generationId = fastGeneration.toStdString();
    rebuiltFastMeta.provider = fastProvider.toStdString();

    auto rebuiltFastIndex = std::unique_ptr<VectorIndex>{};
    if (fastEmbeddingAvailable) {
        rebuiltFastIndex = std::make_unique<VectorIndex>(rebuiltFastMeta);
        if (!rebuiltFastIndex->create()) {
            rebuiltFastIndex.reset();
        }
    }

    VectorStore workerVectorStore(db);
    VectorStore::GenerationState buildingState;
    buildingState.generationId = targetGeneration.toStdString();
    buildingState.modelId = rebuiltMeta.modelId;
    buildingState.dimensions = embeddingDimensions;
    buildingState.provider = rebuiltMeta.provider;
    buildingState.state = "building";
    buildingState.progressPct = 0.0;
    buildingState.active = false;
    workerVectorStore.upsertGenerationState(buildingState);
    if (fastEmbeddingAvailable && rebuiltFastIndex) {
        VectorStore::GenerationState fastBuildingState;
        fastBuildingState.generationId = fastGeneration.toStdString();
        fastBuildingState.modelId = rebuiltFastMeta.modelId;
        fastBuildingState.dimensions = fastEmbeddingDimensions;
        fastBuildingState.provider = rebuiltFastMeta.provider;
        fastBuildingState.state = "building";
        fastBuildingState.progressPct = 0.0;
        fastBuildingState.active = false;
        workerVectorStore.upsertGenerationState(fastBuildingState);
    }

    QString whereClause = QStringLiteral("length(trim(c.chunk_text)) > 0");
    if (!includePaths.isEmpty()) {
        QStringList pathPredicates;
        pathPredicates.reserve(includePaths.size());
        for (int i = 0; i < includePaths.size(); ++i) {
            pathPredicates.append(
                QStringLiteral("(i.path = ?%1 OR i.path LIKE ?%2)")
                    .arg((i * 2) + 1)
                    .arg((i * 2) + 2));
        }
        whereClause += QStringLiteral(" AND (") + pathPredicates.join(QStringLiteral(" OR "))
                       + QStringLiteral(")");
    }

    auto bindScopeParams = [&](sqlite3_stmt* boundStmt) {
        if (!boundStmt || includePaths.isEmpty()) {
            return;
        }
        int bindIndex = 1;
        for (const QString& root : includePaths) {
            const QByteArray rootUtf8 = root.toUtf8();
            QString prefix = root;
            if (prefix == QStringLiteral("/")) {
                prefix = QStringLiteral("/%");
            } else {
                prefix += QStringLiteral("/%");
            }
            const QByteArray prefixUtf8 = prefix.toUtf8();
            sqlite3_bind_text(boundStmt, bindIndex++, rootUtf8.constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(boundStmt, bindIndex++, prefixUtf8.constData(), -1, SQLITE_TRANSIENT);
        }
    };

    const QString countSql = includePaths.isEmpty()
                                 ? QStringLiteral(
                                       "SELECT COUNT(DISTINCT c.item_id) FROM content c "
                                       "WHERE length(trim(c.chunk_text)) > 0")
                                 : QStringLiteral(
                                       "SELECT COUNT(DISTINCT c.item_id) "
                                       "FROM content c "
                                       "JOIN items i ON i.id = c.item_id "
                                       "WHERE %1")
                                       .arg(whereClause);
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db, countSql.toUtf8().constData(), -1, &countStmt, nullptr)
        != SQLITE_OK) {
        updateFailedState(QStringLiteral("Failed to prepare rebuild count query: %1")
                              .arg(QString::fromUtf8(sqlite3_errmsg(db))));
        closeResources();
        return;
    }
    bindScopeParams(countStmt);

    int totalCandidates = 0;
    if (sqlite3_step(countStmt) == SQLITE_ROW) {
        totalCandidates = sqlite3_column_int(countStmt, 0);
    } else {
        sqlite3_finalize(countStmt);
        updateFailedState(QStringLiteral("Failed to evaluate rebuild scope: %1")
                              .arg(QString::fromUtf8(sqlite3_errmsg(db))));
        closeResources();
        return;
    }
    sqlite3_finalize(countStmt);

    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        if (m_vectorRebuildState.runId == runId
            && m_vectorRebuildState.status == VectorRebuildState::Status::Running) {
            m_vectorRebuildState.scopeCandidates = totalCandidates;
            m_vectorRebuildState.totalCandidates = totalCandidates;
        }
    }

    const QString fetchSql = includePaths.isEmpty()
                                 ? QStringLiteral(
                                       "SELECT c.item_id, c.chunk_text "
                                       "FROM content c "
                                       "WHERE length(trim(c.chunk_text)) > 0 "
                                       "ORDER BY c.item_id, c.chunk_index")
                                 : QStringLiteral(
                                       "SELECT c.item_id, c.chunk_text "
                                       "FROM content c "
                                       "JOIN items i ON i.id = c.item_id "
                                       "WHERE %1 "
                                       "ORDER BY c.item_id, c.chunk_index")
                                       .arg(whereClause);
    if (sqlite3_prepare_v2(db, fetchSql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        rollbackIfNeeded();
        updateFailedState(
            QStringLiteral("Failed to prepare rebuild query: %1")
                .arg(QString::fromUtf8(sqlite3_errmsg(db))));
        closeResources();
        return;
    }
    bindScopeParams(stmt);

    int processed = 0;
    int embeddedCount = 0;
    int skippedCount = 0;
    int failedCount = 0;
    struct PendingMapping {
        int64_t itemId = 0;
        uint64_t label = 0;
        int passageOrdinal = 0;
    };
    std::vector<PendingMapping> pendingMappings;
    if (totalCandidates > 0) {
        pendingMappings.reserve(static_cast<size_t>(totalCandidates));
    }
    std::vector<PendingMapping> pendingFastMappings;
    if (totalCandidates > 0) {
        pendingFastMappings.reserve(static_cast<size_t>(totalCandidates));
    }
    int64_t currentItemId = -1;
    QStringList currentChunks;
    qint64 lastPersistedProgressMs = QDateTime::currentMSecsSinceEpoch();
    int lastPersistedProgressProcessed = 0;

    auto publishProgress = [&](bool forcePersist) {
        updateVectorRebuildProgress(runId, totalCandidates, processed,
                                    embeddedCount, skippedCount, failedCount);
        const double progressPct = totalCandidates > 0
            ? (100.0 * static_cast<double>(processed)
               / static_cast<double>(totalCandidates))
            : 100.0;
        {
            std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
            m_vectorMigrationProgressPct = progressPct;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool shouldPersist = forcePersist
            || (processed - lastPersistedProgressProcessed) >= kVectorRebuildProgressPersistStride
            || (nowMs - lastPersistedProgressMs) >= kVectorRebuildProgressPersistIntervalMs;
        if (shouldPersist) {
            buildingState.progressPct = progressPct;
            workerVectorStore.upsertGenerationState(buildingState);
            lastPersistedProgressProcessed = processed;
            lastPersistedProgressMs = nowMs;
        }
    };

    const auto disableInferenceOffloadForRun = [&](const QString& reason) {
        if (!inferenceEmbedOffloadRunEnabled) {
            return;
        }
        inferenceEmbedOffloadRunEnabled = false;
        if (!inferenceFallbackLogged) {
            LOG_WARN(bsIpc,
                     "Vector rebuild run %llu: disabling inference offload (%s); falling back to local embeddings",
                     static_cast<unsigned long long>(runId),
                     qPrintable(reason));
            inferenceFallbackLogged = true;
        }
    };

    auto processCurrentItem = [&]() {
        if (currentItemId <= 0) {
            return;
        }

        ++processed;

        std::vector<QString> texts;
        texts.reserve(static_cast<size_t>(currentChunks.size()));
        for (const QString& chunk : currentChunks) {
            const QString trimmed = chunk.trimmed();
            if (!trimmed.isEmpty()) {
                QString bounded = trimmed;
                if (bounded.size() > kVectorRebuildMaxChunkChars) {
                    bounded.truncate(kVectorRebuildMaxChunkChars);
                }
                texts.push_back(std::move(bounded));
            }
        }
        if (texts.empty()) {
            ++skippedCount;
            return;
        }

        if (m_stopRebuildRequested.load()) return;

        std::vector<std::vector<float>> embeddings;
        if (inferenceEmbedOffloadRunEnabled) {
            embeddings = embedBatchViaInference(QStringLiteral("bi-encoder"), texts, 8);
            if (embeddings.size() != texts.size()) {
                disableInferenceOffloadForRun(
                    QStringLiteral("embed_passages timeout/degraded (bi-encoder)"));
                embeddings.clear();
            }
        }

        if (embeddings.size() != texts.size()) {
            embeddings = embedStrongBatchLocal ? embedStrongBatchLocal(texts)
                                               : std::vector<std::vector<float>>{};
            if (m_stopRebuildRequested.load()) return;
            if (embeddings.size() != texts.size()) {
                embeddings.clear();
                embeddings.reserve(texts.size());
                for (const QString& text : texts) {
                    if (m_stopRebuildRequested.load()) return;
                    embeddings.push_back(
                        embedStrongSingleLocal ? embedStrongSingleLocal(text)
                                               : std::vector<float>{});
                }
            }
        }

        if (m_stopRebuildRequested.load()) return;

        auto normalizeVector = [](const std::vector<float>& source) {
            std::vector<float> normalized = source;
            double normSquared = 0.0;
            for (float value : normalized) {
                normSquared += static_cast<double>(value) * static_cast<double>(value);
            }
            const double norm = std::sqrt(normSquared);
            if (norm > 0.0) {
                for (float& value : normalized) {
                    value = static_cast<float>(static_cast<double>(value) / norm);
                }
            }
            return normalized;
        };

        int usableEmbeddings = 0;
        for (const auto& embedding : embeddings) {
            if (embedding.size() != static_cast<size_t>(embeddingDimensions)) {
                continue;
            }
            std::vector<float> normalized = normalizeVector(embedding);
            const uint64_t label = rebuiltIndex->addVector(normalized.data());
            if (label == std::numeric_limits<uint64_t>::max()) {
                continue;
            }
            pendingMappings.push_back(PendingMapping{
                currentItemId,
                label,
                usableEmbeddings,
            });
            ++usableEmbeddings;
        }

        int usableFastEmbeddings = 0;
        if (fastEmbeddingAvailable && rebuiltFastIndex) {
            std::vector<std::vector<float>> fastEmbeddings;
            if (inferenceEmbedOffloadRunEnabled) {
                fastEmbeddings = embedBatchViaInference(QStringLiteral("bi-encoder-fast"), texts, 8);
                if (fastEmbeddings.size() != texts.size()) {
                    disableInferenceOffloadForRun(
                        QStringLiteral("embed_passages timeout/degraded (bi-encoder-fast)"));
                    fastEmbeddings.clear();
                }
            }

            if (fastEmbeddings.size() != texts.size()) {
                fastEmbeddings = embedFastBatchLocal ? embedFastBatchLocal(texts)
                                                     : std::vector<std::vector<float>>{};
                if (fastEmbeddings.size() != texts.size()) {
                    fastEmbeddings.clear();
                    fastEmbeddings.reserve(texts.size());
                    for (const QString& text : texts) {
                        if (m_stopRebuildRequested.load()) return;
                        fastEmbeddings.push_back(
                            embedFastSingleLocal ? embedFastSingleLocal(text)
                                                 : std::vector<float>{});
                    }
                }
            }

            for (const auto& embedding : fastEmbeddings) {
                if (embedding.size() != static_cast<size_t>(fastEmbeddingDimensions)) {
                    continue;
                }
                std::vector<float> normalized = normalizeVector(embedding);
                const uint64_t label = rebuiltFastIndex->addVector(normalized.data());
                if (label == std::numeric_limits<uint64_t>::max()) {
                    continue;
                }
                pendingFastMappings.push_back(PendingMapping{
                    currentItemId,
                    label,
                    usableFastEmbeddings,
                });
                ++usableFastEmbeddings;
            }
        }

        if (usableEmbeddings == 0 && usableFastEmbeddings == 0) {
            ++failedCount;
            return;
        }
        embeddedCount += usableEmbeddings + usableFastEmbeddings;
    };

    while (true) {
        if (m_stopRebuildRequested.load()) {
            rollbackIfNeeded();
            updateFailedState(QStringLiteral("Vector rebuild cancelled"));
            closeResources();
            return;
        }

        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            rollbackIfNeeded();
            updateFailedState(
                QStringLiteral("Failed during rebuild scan: %1")
                    .arg(QString::fromUtf8(sqlite3_errmsg(db))));
            closeResources();
            return;
        }

        const int64_t itemId = sqlite3_column_int64(stmt, 0);
        const char* rawText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const QString text = rawText ? QString::fromUtf8(rawText) : QString();

        if (currentItemId == -1) {
            currentItemId = itemId;
        }
        if (itemId != currentItemId) {
            processCurrentItem();
            publishProgress(/*forcePersist=*/false);
            currentItemId = itemId;
            currentChunks.clear();
        }

        if (!text.trimmed().isEmpty()
            && currentChunks.size() < kVectorRebuildChunksPerItem) {
            currentChunks.append(text);
        }
    }
    if (currentItemId != -1) {
        processCurrentItem();
    }

    sqlite3_finalize(stmt);
    stmt = nullptr;
    publishProgress(/*forcePersist=*/true);

    if (m_stopRebuildRequested.load()) {
        rollbackIfNeeded();
        updateFailedState(QStringLiteral("Vector rebuild cancelled"));
        closeResources();
        return;
    }

    QDir().mkpath(dataDir);
    const QString tmpIndexPath = indexPath + QStringLiteral(".tmp");
    const QString tmpMetaPath = metaPath + QStringLiteral(".tmp");
    QFile::remove(tmpIndexPath);
    QFile::remove(tmpMetaPath);
    const QString fastIndexPath = dataDir + QStringLiteral("/vectors-") + fastGeneration
        + QStringLiteral(".hnsw");
    const QString fastMetaPath = dataDir + QStringLiteral("/vectors-") + fastGeneration
        + QStringLiteral(".meta");
    const QString tmpFastIndexPath = fastIndexPath + QStringLiteral(".tmp");
    const QString tmpFastMetaPath = fastMetaPath + QStringLiteral(".tmp");
    if (fastEmbeddingAvailable && rebuiltFastIndex) {
        QFile::remove(tmpFastIndexPath);
        QFile::remove(tmpFastMetaPath);
    }

    if (!rebuiltIndex->save(tmpIndexPath.toStdString(), tmpMetaPath.toStdString())) {
        rollbackIfNeeded();
        updateFailedState(QStringLiteral("Failed to save rebuilt vector index"));
        closeResources();
        return;
    }
    if (fastEmbeddingAvailable && rebuiltFastIndex) {
        if (!rebuiltFastIndex->save(tmpFastIndexPath.toStdString(),
                                    tmpFastMetaPath.toStdString())) {
            rollbackIfNeeded();
            updateFailedState(QStringLiteral("Failed to save fast vector index"));
            closeResources();
            return;
        }
    }

    // Keep lock scope short: expensive embedding work happens before this transaction.
    if (!execSql("BEGIN IMMEDIATE TRANSACTION;")) {
        updateFailedState(QStringLiteral("Failed to start rebuild transaction"));
        closeResources();
        return;
    }
    inTransaction = true;

    if (!workerVectorStore.removeGeneration(targetGeneration.toStdString())) {
        rollbackIfNeeded();
        updateFailedState(QStringLiteral("Failed to clear target generation mappings"));
        closeResources();
        return;
    }

    for (const auto& mapping : pendingMappings) {
        if (!workerVectorStore.addMapping(mapping.itemId, mapping.label,
                                          rebuiltMeta.modelId,
                                          targetGeneration.toStdString(),
                                          embeddingDimensions,
                                          rebuiltMeta.provider,
                                          mapping.passageOrdinal,
                                          std::string("active"))) {
            rollbackIfNeeded();
            updateFailedState(QStringLiteral("Failed to persist vector mappings"));
            closeResources();
            return;
        }
    }
    if (fastEmbeddingAvailable && rebuiltFastIndex) {
        if (!workerVectorStore.removeGeneration(fastGeneration.toStdString())) {
            rollbackIfNeeded();
            updateFailedState(QStringLiteral("Failed to clear fast generation mappings"));
            closeResources();
            return;
        }
        for (const auto& mapping : pendingFastMappings) {
            if (!workerVectorStore.addMapping(mapping.itemId, mapping.label,
                                              rebuiltFastMeta.modelId,
                                              fastGeneration.toStdString(),
                                              fastEmbeddingDimensions,
                                              rebuiltFastMeta.provider,
                                              mapping.passageOrdinal,
                                              std::string("active"))) {
                rollbackIfNeeded();
                updateFailedState(QStringLiteral("Failed to persist fast vector mappings"));
                closeResources();
                return;
            }
        }
    }

    auto setSettingWorker = [&](const QString& key, const QString& value) -> bool {
        const char* sql = R"(
            INSERT INTO settings (key, value) VALUES (?1, ?2)
            ON CONFLICT(key) DO UPDATE SET value = excluded.value
        )";

        sqlite3_stmt* settingStmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &settingStmt, nullptr) != SQLITE_OK) {
            return false;
        }
        const QByteArray keyUtf8 = key.toUtf8();
        const QByteArray valUtf8 = value.toUtf8();
        sqlite3_bind_text(settingStmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(settingStmt, 2, valUtf8.constData(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(settingStmt);
        sqlite3_finalize(settingStmt);
        return rc == SQLITE_DONE;
    };

    if (!setSettingWorker(QStringLiteral("nextHnswLabel"),
                          QString::number(rebuiltIndex->nextLabel()))
        || !setSettingWorker(QStringLiteral("hnswDeletedCount"), QStringLiteral("0"))
        || !setSettingWorker(QStringLiteral("activeVectorGeneration"), targetGeneration)
        || !setSettingWorker(QStringLiteral("vectorMigrationState"), QStringLiteral("cutover-complete"))
        || !setSettingWorker(QStringLiteral("vectorMigrationProgressPct"), QStringLiteral("100"))) {
        rollbackIfNeeded();
        updateFailedState(QStringLiteral("Failed to persist vector index settings"));
        closeResources();
        return;
    }

    VectorStore::GenerationState activeState;
    activeState.generationId = targetGeneration.toStdString();
    activeState.modelId = rebuiltMeta.modelId;
    activeState.dimensions = embeddingDimensions;
    activeState.provider = rebuiltMeta.provider;
    activeState.state = "active";
    activeState.progressPct = 100.0;
    activeState.active = true;
    if (!workerVectorStore.upsertGenerationState(activeState)) {
        rollbackIfNeeded();
        updateFailedState(QStringLiteral("Failed to commit generation state"));
        closeResources();
        return;
    }
    if (fastEmbeddingAvailable && rebuiltFastIndex) {
        VectorStore::GenerationState fastState;
        fastState.generationId = fastGeneration.toStdString();
        fastState.modelId = rebuiltFastMeta.modelId;
        fastState.dimensions = fastEmbeddingDimensions;
        fastState.provider = rebuiltFastMeta.provider;
        fastState.state = "active";
        fastState.progressPct = 100.0;
        fastState.active = false;
        if (!workerVectorStore.upsertGenerationState(fastState)) {
            rollbackIfNeeded();
            updateFailedState(QStringLiteral("Failed to commit fast generation state"));
            closeResources();
            return;
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_vectorIndexMutex);
        if (!execSql("COMMIT;")) {
            rollbackIfNeeded();
            updateFailedState(QStringLiteral("Failed to commit vector index rebuild"));
            closeResources();
            return;
        }
        inTransaction = false;
        m_activeVectorGeneration = targetGeneration;
        m_activeVectorModelId = QString::fromStdString(rebuiltMeta.modelId);
        m_activeVectorProvider = QString::fromStdString(rebuiltMeta.provider);
        m_activeVectorDimensions = embeddingDimensions;
        {
            std::lock_guard<std::mutex> stateLock(m_vectorRebuildMutex);
            m_vectorMigrationState = QStringLiteral("cutover-complete");
            m_vectorMigrationProgressPct = 100.0;
        }
        m_vectorIndexPath = indexPath;
        m_vectorMetaPath = metaPath;
        m_vectorIndex = std::move(rebuiltIndex);
        if (fastEmbeddingAvailable && rebuiltFastIndex) {
            m_fastVectorGeneration = fastGeneration;
            m_fastVectorIndexPath = fastIndexPath;
            m_fastVectorMetaPath = fastMetaPath;
            m_fastVectorIndex = std::move(rebuiltFastIndex);
        }
    }

    QString persistError;
    if (QFile::exists(indexPath) && !QFile::remove(indexPath)) {
        persistError = QStringLiteral("Failed to replace %1").arg(indexPath);
    } else if (!QFile::rename(tmpIndexPath, indexPath)) {
        persistError = QStringLiteral("Failed to persist %1").arg(indexPath);
    }

    if (persistError.isEmpty()) {
        if (QFile::exists(metaPath) && !QFile::remove(metaPath)) {
            persistError = QStringLiteral("Failed to replace %1").arg(metaPath);
        } else if (!QFile::rename(tmpMetaPath, metaPath)) {
            persistError = QStringLiteral("Failed to persist %1").arg(metaPath);
        }
    }

    if (persistError.isEmpty() && fastEmbeddingAvailable && QFile::exists(tmpFastIndexPath)
               && QFile::exists(tmpFastMetaPath)) {
        if (QFile::exists(fastIndexPath) && !QFile::remove(fastIndexPath)) {
            persistError = QStringLiteral("Failed to replace %1").arg(fastIndexPath);
        } else if (!QFile::rename(tmpFastIndexPath, fastIndexPath)) {
            persistError = QStringLiteral("Failed to persist %1").arg(fastIndexPath);
        }

        if (persistError.isEmpty()) {
            if (QFile::exists(fastMetaPath) && !QFile::remove(fastMetaPath)) {
                persistError = QStringLiteral("Failed to replace %1").arg(fastMetaPath);
            } else if (!QFile::rename(tmpFastMetaPath, fastMetaPath)) {
                persistError = QStringLiteral("Failed to persist %1").arg(fastMetaPath);
            }
        }
    }

    if (!persistError.isEmpty()) {
        LOG_ERROR(bsIpc, "Vector rebuild run %llu completed but failed to persist index files: %s",
                  static_cast<unsigned long long>(runId), qUtf8Printable(persistError));
    } else {
        LOG_INFO(bsIpc,
                 "Vector index rebuild complete (runId=%llu, total=%d, embedded=%d, skipped=%d, failed=%d, dual=%s)",
                 static_cast<unsigned long long>(runId),
                 totalCandidates, embeddedCount, skippedCount, failedCount,
                 fastEmbeddingAvailable ? "true" : "false");
    }

    updateSucceededState(totalCandidates, processed, embeddedCount, skippedCount,
                         failedCount, persistError);
    closeResources();
}

} // namespace bs
