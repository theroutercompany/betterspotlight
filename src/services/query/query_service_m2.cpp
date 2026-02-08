#include "query_service.h"

#include "core/feedback/feedback_aggregator.h"
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/path_preferences.h"
#include "core/feedback/type_affinity.h"
#include "core/ipc/message.h"
#include "core/shared/logging.h"
#include "core/embedding/embedding_manager.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"

#include <sqlite3.h>

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QJsonArray>

#include <limits>
#include <vector>

namespace bs {

namespace {
constexpr const char* kVectorModelVersion = "bge-small-en-v1.5";
} // namespace

QJsonObject QueryService::handleRecordInteraction(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
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
    if (!ensureStoreOpen()) {
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
    if (!ensureStoreOpen()) {
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
    if (!ensureStoreOpen()) {
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

    if (!ensureStoreOpen()) {
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

QJsonObject QueryService::handleRebuildVectorIndex(uint64_t id)
{
    if (!ensureStoreOpen()) {
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

    sqlite3* db = m_store->rawDb();
    if (!db) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to access database handle"));
    }

    auto rebuiltIndex = std::make_unique<VectorIndex>();
    if (!rebuiltIndex->create()) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to initialize vector index"));
    }

    auto execSql = [db](const char* sql) -> bool {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            LOG_ERROR(bsIpc, "QueryService SQL failed: %s", errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    };

    QElapsedTimer timer;
    timer.start();

    bool inTransaction = false;
    if (!execSql("BEGIN IMMEDIATE TRANSACTION;")) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to start rebuild transaction"));
    }
    inTransaction = true;

    auto rollback = [&]() {
        if (inTransaction) {
            execSql("ROLLBACK;");
            inTransaction = false;
        }
    };

    if (!m_vectorStore->clearAll()) {
        rollback();
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to clear vector mappings"));
    }

    const char* fetchSql = R"(
        SELECT item_id, chunk_text
        FROM content
        WHERE chunk_index = 0
        ORDER BY item_id
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, fetchSql, -1, &stmt, nullptr) != SQLITE_OK) {
        rollback();
        return IpcMessage::makeError(
            id, IpcErrorCode::InternalError,
            QStringLiteral("Failed to prepare rebuild query: %1")
                .arg(QString::fromUtf8(sqlite3_errmsg(db))));
    }

    int totalCandidates = 0;
    int embeddedCount = 0;
    int skippedCount = 0;
    int failedCount = 0;

    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            rollback();
            return IpcMessage::makeError(
                id, IpcErrorCode::InternalError,
                QStringLiteral("Failed during rebuild scan: %1")
                    .arg(QString::fromUtf8(sqlite3_errmsg(db))));
        }

        ++totalCandidates;

        const int64_t itemId = sqlite3_column_int64(stmt, 0);
        const char* rawText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const QString text = rawText ? QString::fromUtf8(rawText) : QString();
        if (text.trimmed().isEmpty()) {
            ++skippedCount;
            continue;
        }

        std::vector<float> embedding = m_embeddingManager->embed(text);
        if (embedding.size() != static_cast<size_t>(VectorIndex::kDimensions)) {
            ++failedCount;
            continue;
        }

        const uint64_t label = rebuiltIndex->addVector(embedding.data());
        if (label == std::numeric_limits<uint64_t>::max()) {
            ++failedCount;
            continue;
        }

        if (!m_vectorStore->addMapping(itemId, label, kVectorModelVersion)) {
            rebuiltIndex->deleteVector(label);
            ++failedCount;
            continue;
        }

        ++embeddedCount;
    }

    sqlite3_finalize(stmt);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                            + QStringLiteral("/betterspotlight");
    QDir().mkpath(dataDir);

    const QString indexPath = dataDir + QStringLiteral("/vectors.hnsw");
    const QString metaPath = dataDir + QStringLiteral("/vectors.meta");
    if (!rebuiltIndex->save(indexPath.toStdString(), metaPath.toStdString())) {
        rollback();
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to save rebuilt vector index"));
    }

    if (!m_store->setSetting(QStringLiteral("nextHnswLabel"),
                             QString::number(rebuiltIndex->nextLabel()))
        || !m_store->setSetting(QStringLiteral("hnswDeletedCount"), QStringLiteral("0"))) {
        rollback();
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to persist vector index settings"));
    }

    if (!execSql("COMMIT;")) {
        rollback();
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to commit vector index rebuild"));
    }
    inTransaction = false;

    m_vectorIndex = std::move(rebuiltIndex);

    LOG_INFO(bsIpc, "Vector index rebuilt: embedded=%d failed=%d skipped=%d",
             embeddedCount, failedCount, skippedCount);

    QJsonObject result;
    result[QStringLiteral("rebuilt")] = true;
    result[QStringLiteral("totalCandidates")] = totalCandidates;
    result[QStringLiteral("embedded")] = embeddedCount;
    result[QStringLiteral("failed")] = failedCount;
    result[QStringLiteral("skipped")] = skippedCount;
    result[QStringLiteral("totalVectors")] = m_vectorIndex ? m_vectorIndex->totalElements() : 0;
    result[QStringLiteral("durationMs")] = timer.elapsed();
    return IpcMessage::makeResponse(id, result);
}

} // namespace bs
