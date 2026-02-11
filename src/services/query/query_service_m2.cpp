#include "query_service.h"

#include "core/feedback/feedback_aggregator.h"
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/path_preferences.h"
#include "core/feedback/type_affinity.h"
#include "core/ipc/message.h"
#include "core/shared/logging.h"
#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"

#include <sqlite3.h>

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace bs {

namespace {
constexpr int kVectorRebuildProgressUpdateStride = 128;
constexpr int kVectorRebuildChunksPerItem = 3;
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

    if (m_pathPreferences) {
        m_pathPreferences->invalidateCache();
    }
    if (m_typeAffinity) {
        m_typeAffinity->invalidateCache();
    }

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
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

QJsonObject QueryService::handleRebuildVectorIndex(uint64_t id,
                                                   const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_embeddingManager || !m_embeddingManager->isAvailable()) {
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

    ModelRegistry workerRegistry(modelsDir);
    EmbeddingManager workerEmbeddingManager(&workerRegistry);
    if (!workerEmbeddingManager.initialize()) {
        updateFailedState(QStringLiteral("Embedding model is unavailable"));
        closeResources();
        return;
    }

    const int embeddingDimensions = std::max(workerEmbeddingManager.embeddingDimensions(), 1);
    VectorIndex::IndexMetadata rebuiltMeta;
    rebuiltMeta.dimensions = embeddingDimensions;
    rebuiltMeta.modelId = workerEmbeddingManager.activeModelId().toStdString();
    rebuiltMeta.generationId = targetGeneration.toStdString();
    rebuiltMeta.provider = workerEmbeddingManager.providerName().toStdString();

    auto rebuiltIndex = std::make_unique<VectorIndex>(rebuiltMeta);
    if (!rebuiltIndex->create()) {
        updateFailedState(QStringLiteral("Failed to initialize vector index"));
        closeResources();
        return;
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
    int64_t currentItemId = -1;
    QStringList currentChunks;

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
                texts.push_back(trimmed);
            }
        }
        if (texts.empty()) {
            ++skippedCount;
            return;
        }

        if (m_stopRebuildRequested.load()) return;

        std::vector<std::vector<float>> embeddings = workerEmbeddingManager.embedBatch(texts);

        if (m_stopRebuildRequested.load()) return;

        if (embeddings.size() != texts.size()) {
            embeddings.clear();
            embeddings.reserve(texts.size());
            for (const QString& text : texts) {
                if (m_stopRebuildRequested.load()) return;
                embeddings.push_back(workerEmbeddingManager.embed(text));
            }
        }

        int usableEmbeddings = 0;
        for (const auto& embedding : embeddings) {
            if (embedding.size() != static_cast<size_t>(embeddingDimensions)) {
                continue;
            }
            std::vector<float> normalized = embedding;
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

        if (usableEmbeddings == 0) {
            ++failedCount;
            return;
        }
        embeddedCount += usableEmbeddings;
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
            if (processed % kVectorRebuildProgressUpdateStride == 0) {
                updateVectorRebuildProgress(runId, totalCandidates, processed,
                                            embeddedCount, skippedCount, failedCount);
                const double progressPct = totalCandidates > 0
                    ? (100.0 * static_cast<double>(processed)
                       / static_cast<double>(totalCandidates))
                    : 0.0;
                {
                    std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
                    m_vectorMigrationProgressPct = progressPct;
                }
                buildingState.progressPct = progressPct;
                workerVectorStore.upsertGenerationState(buildingState);
            }
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
    updateVectorRebuildProgress(runId, totalCandidates, processed,
                                embeddedCount, skippedCount, failedCount);
    const double finalProgressPct = totalCandidates > 0
        ? (100.0 * static_cast<double>(processed)
           / static_cast<double>(totalCandidates))
        : 100.0;
    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        m_vectorMigrationProgressPct = finalProgressPct;
    }
    buildingState.progressPct = finalProgressPct;
    workerVectorStore.upsertGenerationState(buildingState);

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

    if (!rebuiltIndex->save(tmpIndexPath.toStdString(), tmpMetaPath.toStdString())) {
        rollbackIfNeeded();
        updateFailedState(QStringLiteral("Failed to save rebuilt vector index"));
        closeResources();
        return;
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

    if (!persistError.isEmpty()) {
        LOG_ERROR(bsIpc, "Vector rebuild run %llu completed but failed to persist index files: %s",
                  static_cast<unsigned long long>(runId), qUtf8Printable(persistError));
    } else {
        LOG_INFO(bsIpc,
                 "Vector index rebuild complete (runId=%llu, total=%d, embedded=%d, skipped=%d, failed=%d)",
                 static_cast<unsigned long long>(runId),
                 totalCandidates, embeddedCount, skippedCount, failedCount);
    }

    updateSucceededState(totalCandidates, processed, embeddedCount, skippedCount,
                         failedCount, persistError);
    closeResources();
}

} // namespace bs
