#include "query_service.h"
#include "core/ipc/message.h"
#include "core/shared/search_result.h"
#include "core/shared/logging.h"

#include <sqlite3.h>

#include "core/embedding/embedding_manager.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"
#include "core/vector/search_merger.h"
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/feedback_aggregator.h"
#include "core/feedback/path_preferences.h"
#include "core/feedback/type_affinity.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QStandardPaths>

namespace bs {

static std::vector<Highlight> parseHighlights(const QString& snippet)
{
    std::vector<Highlight> highlights;
    const QString openTag = QStringLiteral("<b>");
    const QString closeTag = QStringLiteral("</b>");

    int plainOffset = 0;
    int pos = 0;

    while (pos < snippet.size()) {
        int openPos = snippet.indexOf(openTag, pos);
        if (openPos == -1) {
            plainOffset += snippet.size() - pos;
            break;
        }

        plainOffset += openPos - pos;

        int contentStart = openPos + openTag.size();
        int closePos = snippet.indexOf(closeTag, contentStart);
        if (closePos == -1) {
            plainOffset += snippet.size() - openPos;
            break;
        }

        int highlightLen = closePos - contentStart;
        Highlight h;
        h.offset = plainOffset;
        h.length = highlightLen;
        highlights.push_back(h);

        plainOffset += highlightLen;
        pos = closePos + closeTag.size();
    }

    return highlights;
}

QueryService::QueryService(QObject* parent)
    : ServiceBase(QStringLiteral("query"), parent)
{
    LOG_INFO(bsIpc, "QueryService created");
}

QueryService::~QueryService() = default;

QJsonObject QueryService::handleRequest(const QJsonObject& request)
{
    QString method = request.value(QStringLiteral("method")).toString();
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
    QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("search"))          return handleSearch(id, params);
    if (method == QLatin1String("getHealth"))        return handleGetHealth(id);
    if (method == QLatin1String("recordFeedback"))   return handleRecordFeedback(id, params);
    if (method == QLatin1String("getFrequency"))     return handleGetFrequency(id, params);

    if (method == QLatin1String("record_interaction"))       return handleRecordInteraction(id, params);
    if (method == QLatin1String("get_path_preferences"))     return handleGetPathPreferences(id, params);
    if (method == QLatin1String("get_file_type_affinity"))   return handleGetFileTypeAffinity(id);
    if (method == QLatin1String("run_aggregation"))          return handleRunAggregation(id);
    if (method == QLatin1String("export_interaction_data"))  return handleExportInteractionData(id, params);

    return ServiceBase::handleRequest(request);
}

bool QueryService::ensureStoreOpen()
{
    if (m_store.has_value()) {
        return true;
    }

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/betterspotlight");
    QString dbPath = dataDir + QStringLiteral("/index.db");

    auto store = SQLiteStore::open(dbPath);
    if (!store.has_value()) {
        LOG_ERROR(bsIpc, "Failed to open database at: %s", qPrintable(dbPath));
        return false;
    }

    m_store.emplace(std::move(store.value()));
    LOG_INFO(bsIpc, "Database opened at: %s", qPrintable(dbPath));

    initM2Modules();
    return true;
}

void QueryService::initM2Modules()
{
    if (m_m2Initialized) {
        return;
    }
    m_m2Initialized = true;

    ::sqlite3* db = m_store->rawDb();

    m_interactionTracker = std::make_unique<InteractionTracker>(db);
    m_feedbackAggregator = std::make_unique<FeedbackAggregator>(db);
    m_pathPreferences = std::make_unique<PathPreferences>(db);
    m_typeAffinity = std::make_unique<TypeAffinity>(db);

    m_vectorStore = std::make_unique<VectorStore>(db);

    m_vectorIndex = std::make_unique<VectorIndex>();
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/betterspotlight");
    QString indexPath = dataDir + QStringLiteral("/vectors.hnsw");
    QString metaPath = dataDir + QStringLiteral("/vectors.meta");

    if (QFile::exists(indexPath) && QFile::exists(metaPath)) {
        if (!m_vectorIndex->load(indexPath.toStdString(), metaPath.toStdString())) {
            LOG_WARN(bsIpc, "Failed to load vector index, semantic search disabled");
            m_vectorIndex.reset();
        } else {
            LOG_INFO(bsIpc, "Vector index loaded: %d vectors", m_vectorIndex->totalElements());
        }
    } else {
        if (!m_vectorIndex->create()) {
            LOG_WARN(bsIpc, "Failed to create vector index, semantic search disabled");
            m_vectorIndex.reset();
        }
    }

    QString modelDir = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/../Resources/models");
    QString modelPath = modelDir + QStringLiteral("/bge-small-en-v1.5-int8.onnx");
    QString vocabPath = modelDir + QStringLiteral("/vocab.txt");

    m_embeddingManager = std::make_unique<EmbeddingManager>(modelPath, vocabPath);
    if (!m_embeddingManager->initialize()) {
        LOG_WARN(bsIpc, "EmbeddingManager unavailable, semantic search disabled");
    } else {
        LOG_INFO(bsIpc, "EmbeddingManager initialized");
    }
}

QJsonObject QueryService::handleSearch(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    // Parse query
    QString query = params.value(QStringLiteral("query")).toString();
    if (query.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'query' parameter"));
    }

    // Parse limit (default 20)
    int limit = 20;
    if (params.contains(QStringLiteral("limit"))) {
        limit = params.value(QStringLiteral("limit")).toInt(20);
        if (limit < 1) {
            limit = 1;
        } else if (limit > 200) {
            limit = 200;
        }
    }

    // Parse context
    QueryContext context;
    if (params.contains(QStringLiteral("context"))) {
        QJsonObject ctxObj = params.value(QStringLiteral("context")).toObject();
        if (ctxObj.contains(QStringLiteral("cwdPath"))) {
            context.cwdPath = ctxObj.value(QStringLiteral("cwdPath")).toString();
        }
        if (ctxObj.contains(QStringLiteral("frontmostAppBundleId"))) {
            context.frontmostAppBundleId = ctxObj.value(
                QStringLiteral("frontmostAppBundleId")).toString();
        }
        if (ctxObj.contains(QStringLiteral("recentPaths"))) {
            QJsonArray recentArr = ctxObj.value(QStringLiteral("recentPaths")).toArray();
            context.recentPaths.reserve(static_cast<size_t>(recentArr.size()));
            for (const auto& val : recentArr) {
                context.recentPaths.push_back(val.toString());
            }
        }
    }

    LOG_INFO(bsIpc, "Search: query='%s' limit=%d", qPrintable(query), limit);

    QElapsedTimer timer;
    timer.start();

    // Overquery for ranking: fetch limit * 2 from FTS5
    int ftsLimit = limit * 2;
    std::vector<SQLiteStore::FtsHit> hits = m_store->searchFts5(query, ftsLimit);
    int totalMatches = static_cast<int>(hits.size());

    // Build SearchResult list from FTS hits
    std::vector<SearchResult> results;
    results.reserve(hits.size());

    for (const auto& hit : hits) {
        auto itemOpt = m_store->getItemById(hit.fileId);
        if (!itemOpt.has_value()) {
            continue;
        }

        const auto& item = itemOpt.value();

        SearchResult sr;
        sr.itemId = item.id;
        sr.path = item.path;
        sr.name = item.name;
        sr.kind = item.kind;
        sr.snippet = hit.snippet;
        sr.highlights = parseHighlights(sr.snippet);
        sr.fileSize = item.size;

        // Format modification date as ISO 8601
        if (item.modifiedAt > 0.0) {
            sr.modificationDate = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(item.modifiedAt * 1000.0)).toUTC().toString(Qt::ISODate);
        }

        sr.isPinned = item.isPinned;

        // Query frequency data for this item
        auto freqOpt = m_store->getFrequency(item.id);
        if (freqOpt.has_value()) {
            sr.openCount = freqOpt->openCount;
            if (freqOpt->lastOpenedAt > 0.0) {
                sr.lastOpenDate = QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(freqOpt->lastOpenedAt * 1000.0))
                    .toUTC().toString(Qt::ISODate);
            }
        }

        // Classify match type for name/path matches
        sr.matchType = MatchClassifier::classify(query, item.name, item.path);

        results.push_back(std::move(sr));
    }

    // Apply multi-signal ranking (M1 base scoring)
    m_scorer.rankResults(results, context);

    // M2: Semantic search + merge
    bool semanticAvailable = m_embeddingManager && m_embeddingManager->isAvailable()
                             && m_vectorIndex && m_vectorIndex->isAvailable();

    if (semanticAvailable) {
        std::vector<float> queryVec = m_embeddingManager->embedQuery(query);
        if (!queryVec.empty()) {
            auto knnHits = m_vectorIndex->search(queryVec.data());

            std::vector<SemanticResult> semanticResults;
            semanticResults.reserve(knnHits.size());
            for (const auto& hit : knnHits) {
                float cosineSim = 1.0f - hit.distance;
                if (cosineSim < 0.7f) {
                    continue;
                }
                auto itemIdOpt = m_vectorStore->getItemId(hit.label);
                if (!itemIdOpt.has_value()) {
                    continue;
                }
                SemanticResult sr;
                sr.itemId = itemIdOpt.value();
                sr.cosineSimilarity = cosineSim;
                semanticResults.push_back(sr);
            }

            if (!semanticResults.empty()) {
                MergeConfig mergeConfig;
                results = SearchMerger::merge(results, semanticResults, mergeConfig);
            }
        }
    }

    // M2: Apply interaction, path preference, and type affinity boosts
    QString normalizedQuery = InteractionTracker::normalizeQuery(query);
    for (auto& sr : results) {
        double m2Boost = 0.0;

        if (m_interactionTracker) {
            m2Boost += m_interactionTracker->getInteractionBoost(normalizedQuery, sr.itemId);
        }
        if (m_pathPreferences) {
            m2Boost += m_pathPreferences->getBoost(sr.path);
        }
        if (m_typeAffinity) {
            m2Boost += m_typeAffinity->getBoost(sr.path);
        }

        sr.score += m2Boost;
    }

    // Re-sort after M2 boosts
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.itemId < b.itemId;
    });

    // Truncate to the requested limit
    if (static_cast<int>(results.size()) > limit) {
        results.resize(static_cast<size_t>(limit));
    }

    // Serialize results to JSON array
    QJsonArray resultsArray;
    for (const auto& sr : results) {
        QJsonObject metadata;
        metadata[QStringLiteral("fileSize")] = static_cast<qint64>(sr.fileSize);
        metadata[QStringLiteral("modificationDate")] = sr.modificationDate;

        QJsonObject frequency;
        frequency[QStringLiteral("openCount")] = sr.openCount;
        frequency[QStringLiteral("lastOpenDate")] = sr.lastOpenDate;

        QJsonObject obj;
        obj[QStringLiteral("itemId")] = static_cast<qint64>(sr.itemId);
        obj[QStringLiteral("path")] = sr.path;
        obj[QStringLiteral("name")] = sr.name;
        obj[QStringLiteral("kind")] = sr.kind;
        obj[QStringLiteral("matchType")] = matchTypeToString(sr.matchType);
        obj[QStringLiteral("score")] = sr.score;

        QString plainSnippet = sr.snippet;
        plainSnippet.replace(QStringLiteral("<b>"), QString());
        plainSnippet.replace(QStringLiteral("</b>"), QString());
        obj[QStringLiteral("snippet")] = plainSnippet;

        QJsonArray highlightsArray;
        for (const auto& highlight : sr.highlights) {
            QJsonObject highlightObj;
            highlightObj[QStringLiteral("offset")] = highlight.offset;
            highlightObj[QStringLiteral("length")] = highlight.length;
            highlightsArray.append(highlightObj);
        }
        obj[QStringLiteral("highlights")] = highlightsArray;
        obj[QStringLiteral("metadata")] = metadata;
        obj[QStringLiteral("isPinned")] = sr.isPinned;
        obj[QStringLiteral("frequency")] = frequency;
        resultsArray.append(obj);
    }

    QJsonObject result;
    result[QStringLiteral("results")] = resultsArray;
    result[QStringLiteral("queryTime")] = static_cast<int>(timer.elapsed());
    result[QStringLiteral("totalMatches")] = totalMatches;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetHealth(uint64_t id)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    IndexHealth health = m_store->getHealth();

    QJsonObject indexHealth;
    indexHealth[QStringLiteral("isHealthy")] = health.isHealthy;
    indexHealth[QStringLiteral("totalIndexedItems")] = static_cast<qint64>(health.totalIndexedItems);
    indexHealth[QStringLiteral("totalChunks")] = static_cast<qint64>(health.totalChunks);
    indexHealth[QStringLiteral("totalFailures")] = static_cast<qint64>(health.totalFailures);
    indexHealth[QStringLiteral("lastIndexTime")] = health.lastIndexTime;
    indexHealth[QStringLiteral("indexAge")] = health.indexAge;
    indexHealth[QStringLiteral("ftsIndexSize")] = static_cast<qint64>(health.ftsIndexSize);
    indexHealth[QStringLiteral("itemsWithoutContent")] = static_cast<qint64>(health.itemsWithoutContent);

    QJsonObject serviceHealth;
    serviceHealth[QStringLiteral("indexerRunning")] = true;
    serviceHealth[QStringLiteral("extractorRunning")] = true;
    serviceHealth[QStringLiteral("queryServiceRunning")] = true;
    serviceHealth[QStringLiteral("uptime")] = 0;

    QJsonObject result;
    result[QStringLiteral("indexHealth")] = indexHealth;
    result[QStringLiteral("serviceHealth")] = serviceHealth;
    result[QStringLiteral("issues")] = QJsonArray();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleRecordFeedback(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    // Parse required fields
    if (!params.contains(QStringLiteral("itemId"))) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'itemId' parameter"));
    }

    int64_t itemId = static_cast<int64_t>(params.value(QStringLiteral("itemId")).toInteger());
    QString action = params.value(QStringLiteral("action")).toString();
    QString query = params.value(QStringLiteral("query")).toString();
    int position = params.value(QStringLiteral("position")).toInt(0);

    // Record feedback in the feedback table
    if (!m_store->recordFeedback(itemId, action, query, position)) {
        LOG_WARN(bsIpc, "Failed to insert feedback row for item %lld",
                 static_cast<long long>(itemId));
    }

    // Also update frequency counters
    if (!m_store->incrementFrequency(itemId)) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to record feedback for item %1")
                                         .arg(itemId));
    }

    LOG_INFO(bsIpc, "Feedback recorded for item %lld", static_cast<long long>(itemId));

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetFrequency(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!params.contains(QStringLiteral("itemId"))) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'itemId' parameter"));
    }

    int64_t itemId = static_cast<int64_t>(params.value(QStringLiteral("itemId")).toInteger());

    auto freqOpt = m_store->getFrequency(itemId);

    int openCount = 0;
    QString lastOpenDate;
    if (freqOpt.has_value()) {
        openCount = freqOpt->openCount;
        if (freqOpt->lastOpenedAt > 0.0) {
            lastOpenDate = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(freqOpt->lastOpenedAt * 1000.0))
                .toUTC().toString(Qt::ISODate);
        }
    }

    // Compute frequency tier: 0 opens = tier 0, 1-5 = tier 1, 6-20 = tier 2, 21+ = tier 3
    int frequencyTier = 0;
    if (openCount >= 21) {
        frequencyTier = 3;
    } else if (openCount >= 6) {
        frequencyTier = 2;
    } else if (openCount >= 1) {
        frequencyTier = 1;
    }

    // Compute boost using scorer
    double boost = m_scorer.computeFrequencyBoost(
        openCount, freqOpt.has_value() ? freqOpt->lastOpenedAt : 0.0);

    QJsonObject result;
    result[QStringLiteral("openCount")] = openCount;
    result[QStringLiteral("lastOpenDate")] = lastOpenDate;
    result[QStringLiteral("frequencyTier")] = frequencyTier;
    result[QStringLiteral("boost")] = boost;
    return IpcMessage::makeResponse(id, result);
}

} // namespace bs
