#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QThread>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace bs {

class SQLiteStore;
class EmbeddingManager;
class VectorIndex;
class VectorStore;

class EmbeddingPipeline : public QObject {
    Q_OBJECT

public:
    explicit EmbeddingPipeline(SQLiteStore* store,
                               EmbeddingManager* embeddingManager,
                               VectorIndex* vectorIndex,
                               VectorStore* vectorStore,
                               QObject* parent = nullptr);
    ~EmbeddingPipeline() override;

    EmbeddingPipeline(const EmbeddingPipeline&) = delete;
    EmbeddingPipeline& operator=(const EmbeddingPipeline&) = delete;
    EmbeddingPipeline(EmbeddingPipeline&&) = delete;
    EmbeddingPipeline& operator=(EmbeddingPipeline&&) = delete;

    void start();
    void stop();
    void pause();
    void resume();

    bool isRunning() const;
    int processedCount() const;

signals:
    void progressUpdated(int processed, int total);
    void finished();
    void error(const QString& message);

private:
    struct UnembeddedItem {
        int64_t itemId = 0;
        QString content;
    };

    void run();
    int countUnembeddedItems();
    std::vector<UnembeddedItem> fetchUnembeddedBatch(int limit);
    void processBatch(const std::vector<UnembeddedItem>& batch);
    bool processSingleEmbedding(int64_t itemId, const std::vector<float>& embedding);
    bool shouldSave() const;
    void saveIndex();
    int currentBatchSize() const;
    int processRssMb() const;

    SQLiteStore* m_store = nullptr;
    EmbeddingManager* m_embeddingManager = nullptr;
    VectorIndex* m_vectorIndex = nullptr;
    VectorStore* m_vectorStore = nullptr;

    std::unique_ptr<QThread> m_workerThread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_pauseRequested{false};
    std::atomic<int> m_processedCount{0};

    int m_totalCount = 0;
    int m_itemsSinceLastSave = 0;
    QElapsedTimer m_lastSaveTime;

    static constexpr int kBatchSize = 32;
    static constexpr int kMinBatchSize = 4;
    static constexpr int kIdleSleepMs = 500;
    static constexpr int kSaveItemThreshold = 1000;
    static constexpr qint64 kSaveTimeThresholdMs = 60000;
};

} // namespace bs
