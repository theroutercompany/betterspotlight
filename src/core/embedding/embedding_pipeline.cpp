#include "core/embedding/embedding_pipeline.h"

#include "core/embedding/embedding_manager.h"
#include "core/embedding/quantizer.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"

#include "vendor/sqlite/sqlite3.h"

#include <QDir>
#include <QStandardPaths>

#include <limits>

namespace bs {

namespace {

constexpr const char* kModelVersion = "bge-small-en-v1.5";

sqlite3* rawDbFromStore(const SQLiteStore* store)
{
    if (store == nullptr) {
        return nullptr;
    }
    return *reinterpret_cast<sqlite3* const*>(store);
}

constexpr const char* kCountUnembeddedSql = R"(
    SELECT COUNT(*)
    FROM items i
    LEFT JOIN vector_map vm ON i.id = vm.item_id
    INNER JOIN chunks c ON i.id = c.file_id
    WHERE vm.item_id IS NULL
      AND c.chunk_index = 0
)";

constexpr const char* kFetchUnembeddedSql = R"(
    SELECT i.id, c.content
    FROM items i
    LEFT JOIN vector_map vm ON i.id = vm.item_id
    INNER JOIN chunks c ON i.id = c.file_id
    WHERE vm.item_id IS NULL
      AND c.chunk_index = 0
    LIMIT ?1
)";

} // namespace

EmbeddingPipeline::EmbeddingPipeline(SQLiteStore* store,
                                     EmbeddingManager* embeddingManager,
                                     Quantizer* quantizer,
                                     VectorIndex* vectorIndex,
                                     VectorStore* vectorStore,
                                     QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_embeddingManager(embeddingManager)
    , m_quantizer(quantizer)
    , m_vectorIndex(vectorIndex)
    , m_vectorStore(vectorStore)
{
}

EmbeddingPipeline::~EmbeddingPipeline()
{
    stop();
}

void EmbeddingPipeline::start()
{
    if (isRunning()) {
        return;
    }

    if (m_store == nullptr || m_embeddingManager == nullptr || m_quantizer == nullptr
        || m_vectorIndex == nullptr || m_vectorStore == nullptr) {
        emit error(QStringLiteral("EmbeddingPipeline start failed: missing dependency"));
        return;
    }

    m_stopRequested.store(false);
    m_pauseRequested.store(false);
    m_processedCount.store(0);
    m_totalCount = countUnembeddedItems();
    m_itemsSinceLastSave = 0;
    m_lastSaveTime.start();

    m_workerThread.reset(QThread::create([this]() {
        run();
    }));

    m_workerThread->start(QThread::LowPriority);
}

void EmbeddingPipeline::stop()
{
    m_stopRequested.store(true);
    m_pauseRequested.store(false);

    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->wait();
    }

    m_workerThread.reset();
}

void EmbeddingPipeline::pause()
{
    m_pauseRequested.store(true);
}

void EmbeddingPipeline::resume()
{
    m_pauseRequested.store(false);
}

bool EmbeddingPipeline::isRunning() const
{
    return m_workerThread && m_workerThread->isRunning();
}

int EmbeddingPipeline::processedCount() const
{
    return m_processedCount.load();
}

void EmbeddingPipeline::run()
{
    bool didEmitFinished = false;

    while (!m_stopRequested.load()) {
        if (m_pauseRequested.load()) {
            QThread::msleep(kIdleSleepMs);
            continue;
        }

        const std::vector<UnembeddedItem> batch = fetchUnembeddedBatch(kBatchSize);
        if (batch.empty()) {
            didEmitFinished = true;
            emit finished();
            break;
        }

        processBatch(batch);

        m_processedCount.fetch_add(static_cast<int>(batch.size()));
        m_itemsSinceLastSave += static_cast<int>(batch.size());
        emit progressUpdated(m_processedCount.load(), m_totalCount);

        if (shouldSave()) {
            saveIndex();
        }

        QThread::msleep(kIdleSleepMs);
    }

    if (m_itemsSinceLastSave > 0) {
        saveIndex();
    }

    if (!didEmitFinished) {
        emit finished();
    }
}

int EmbeddingPipeline::countUnembeddedItems()
{
    sqlite3* db = rawDbFromStore(m_store);
    if (db == nullptr) {
        emit error(QStringLiteral("EmbeddingPipeline count failed: null SQLite handle"));
        return 0;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kCountUnembeddedSql, -1, &stmt, nullptr) != SQLITE_OK) {
        emit error(QStringLiteral("EmbeddingPipeline count prepare failed: %1")
                       .arg(QString::fromUtf8(sqlite3_errmsg(db))));
        return 0;
    }

    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(stmt, 0);
    } else {
        emit error(QStringLiteral("EmbeddingPipeline count step failed: %1")
                       .arg(QString::fromUtf8(sqlite3_errmsg(db))));
    }

    sqlite3_finalize(stmt);
    return total;
}

std::vector<EmbeddingPipeline::UnembeddedItem> EmbeddingPipeline::fetchUnembeddedBatch(int limit)
{
    std::vector<UnembeddedItem> result;
    if (limit <= 0) {
        return result;
    }

    sqlite3* db = rawDbFromStore(m_store);
    if (db == nullptr) {
        emit error(QStringLiteral("EmbeddingPipeline fetch failed: null SQLite handle"));
        return result;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kFetchUnembeddedSql, -1, &stmt, nullptr) != SQLITE_OK) {
        emit error(QStringLiteral("EmbeddingPipeline fetch prepare failed: %1")
                       .arg(QString::fromUtf8(sqlite3_errmsg(db))));
        return result;
    }

    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t itemId = sqlite3_column_int64(stmt, 0);
        const unsigned char* contentRaw = sqlite3_column_text(stmt, 1);
        const QString content = contentRaw
                                    ? QString::fromUtf8(reinterpret_cast<const char*>(contentRaw))
                                    : QString();
        result.push_back(UnembeddedItem{itemId, content});
    }

    sqlite3_finalize(stmt);
    return result;
}

void EmbeddingPipeline::processBatch(const std::vector<UnembeddedItem>& batch)
{
    if (batch.empty()) {
        return;
    }

    std::vector<QString> texts;
    texts.reserve(batch.size());
    for (const UnembeddedItem& item : batch) {
        texts.push_back(item.content);
    }

    const std::vector<std::vector<float>> embeddings = m_embeddingManager->embedBatch(texts);
    const bool batchSucceeded = embeddings.size() == batch.size();

    if (!batchSucceeded) {
        emit error(QStringLiteral("EmbeddingPipeline batch inference failed, retrying individually"));
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        std::vector<float> embedding;
        if (batchSucceeded) {
            embedding = embeddings[i];
        } else {
            embedding = m_embeddingManager->embed(batch[i].content);
            if (embedding.empty()) {
                qWarning() << "EmbeddingPipeline individual embed failed for item" << batch[i].itemId;
                continue;
            }
        }

        if (!processSingleEmbedding(batch[i].itemId, embedding)) {
            qWarning() << "EmbeddingPipeline failed to persist embedding for item" << batch[i].itemId;
        }
    }
}

bool EmbeddingPipeline::processSingleEmbedding(int64_t itemId, const std::vector<float>& embedding)
{
    if (embedding.size() != static_cast<size_t>(VectorIndex::kDimensions)) {
        qWarning() << "EmbeddingPipeline invalid embedding size for item" << itemId
                   << "expected" << VectorIndex::kDimensions
                   << "got" << embedding.size();
        return false;
    }

    const QuantizedVector quantized = m_quantizer->quantize(embedding);
    if (quantized.data.size() != static_cast<size_t>(Quantizer::kEmbeddingDimensions)) {
        qWarning() << "EmbeddingPipeline quantization failed for item" << itemId;
        return false;
    }

    const uint64_t label = m_vectorIndex->addVector(embedding.data());
    if (label == std::numeric_limits<uint64_t>::max()) {
        qWarning() << "EmbeddingPipeline addVector failed for item" << itemId;
        return false;
    }

    if (!m_vectorStore->addMapping(itemId, label, kModelVersion)) {
        qWarning() << "EmbeddingPipeline addMapping failed for item" << itemId;
        m_vectorIndex->deleteVector(label);
        return false;
    }

    return true;
}

bool EmbeddingPipeline::shouldSave() const
{
    if (m_itemsSinceLastSave >= kSaveItemThreshold) {
        return true;
    }
    if (!m_lastSaveTime.isValid()) {
        return false;
    }
    return m_lastSaveTime.elapsed() >= kSaveTimeThresholdMs;
}

void EmbeddingPipeline::saveIndex()
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/betterspotlight");
    QDir().mkpath(dataDir);

    const QString indexPath = dataDir + QStringLiteral("/vectors.hnsw");
    const QString metaPath = dataDir + QStringLiteral("/vectors.meta");
    if (!m_vectorIndex->save(indexPath.toStdString(), metaPath.toStdString())) {
        emit error(QStringLiteral("EmbeddingPipeline failed saving vector index"));
        return;
    }

    m_itemsSinceLastSave = 0;
    m_lastSaveTime.restart();
}

} // namespace bs
