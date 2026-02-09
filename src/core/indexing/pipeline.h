#pragma once

#include "core/indexing/chunker.h"
#include "core/indexing/indexer.h"
#include "core/indexing/work_queue.h"
#include "core/fs/file_monitor_macos.h"
#include "core/fs/path_rules.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bs {

class SQLiteStore;
class ExtractionManager;

// Pipeline — top-level indexing orchestrator.
//
// Architecture:
//  1) Ingress/Coordinator (WorkQueue + generation tracking)
//  2) Prep worker pool (validation, stat, extraction, chunking)
//  3) Single DB writer (SQLite mutations + batched commits)
class Pipeline : public QObject {
    Q_OBJECT

public:
    Pipeline(SQLiteStore& store, ExtractionManager& extractor,
             const PathRules& pathRules, QObject* parent = nullptr);
    ~Pipeline() override;

    // Non-copyable, non-movable (QObject + thread ownership)
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    // Start monitoring the given root directories and processing items.
    void start(const std::vector<std::string>& roots);

    // Stop monitoring and processing. Blocks until all worker threads exit.
    void stop();

    // Pause/resume ingestion and prep scheduling.
    void pause();
    void resume();

    // Enqueue a single path for re-indexing.
    void reindexPath(const QString& path);

    // Drop all indexed data and re-scan from scratch.
    void rebuildAll(const std::vector<std::string>& roots);

    // Hint that the user is actively interacting (e.g. typing a query).
    // Active mode clamps prep concurrency to one worker.
    void setUserActive(bool active);

    // Snapshot of current queue statistics.
    QueueStats queueStatus() const;

    // Number of items processed so far.
    int processedCount() const { return m_processedCount.load(); }

signals:
    void progressUpdated(int processedCount, int totalCount);
    void indexingComplete();
    void indexingError(const QString& error);

private:
    struct PrepTask {
        WorkItem item;
        uint64_t generation = 0;
    };

    struct PathCoordinatorState {
        uint64_t latestGeneration = 0;
        bool inPrep = false;
        std::optional<WorkItem::Type> pendingMergedType;
    };

    // Scan thread entry point: walks directories and enqueues work items.
    void scanEntry();

    // Stage loops.
    void prepDispatcherLoop();
    void prepWorkerLoop(size_t workerIndex);
    void writerLoop();

    // Callback from FileMonitorMacOS — enqueues incoming FS events.
    void onFileSystemEvents(const std::vector<WorkItem>& items);

    // Coordinator helpers.
    std::optional<PrepTask> tryDispatchFromIngress(const WorkItem& item);
    std::optional<PrepTask> onPrepCompleted(const PreparedWork& prepared);
    bool isStalePreparedWork(const PreparedWork& prepared) const;

    // Runtime helpers.
    void resetRuntimeState();
    void wakeAllStages();
    void waitForPipelineDrain();
    size_t pendingMergedCount() const;
    size_t totalPendingDepth() const;
    static WorkItem::Type mergeWorkTypes(WorkItem::Type lhs, WorkItem::Type rhs);
    bool waitForScanBackpressureWindow() const;
    bool enqueuePrimaryWorkItem(const WorkItem& item, int maxAttempts = 1200);

    // Concurrency policy helpers.
    void updatePrepConcurrencyPolicy();
    static size_t computeIdlePrepWorkers();

    SQLiteStore& m_store;
    ExtractionManager& m_extractor;
    PathRules m_pathRules;
    Chunker m_chunker;
    WorkQueue m_workQueue;
    std::unique_ptr<Indexer> m_indexer;
    std::unique_ptr<FileMonitorMacOS> m_monitor;

    // Threads
    std::thread m_scanThread;
    std::thread m_dispatchThread;
    std::vector<std::thread> m_prepThreads;
    std::thread m_writerThread;

    // Lifecycle flags
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_paused{false};

    // Stage queues
    mutable std::mutex m_prepMutex;
    std::condition_variable m_prepCv;
    std::deque<PrepTask> m_prepQueue;

    mutable std::mutex m_preparedMutex;
    std::condition_variable m_preparedCv;
    std::deque<PreparedWork> m_preparedQueue;

    // Path coordinator state
    mutable std::mutex m_coordMutex;
    std::unordered_map<std::string, PathCoordinatorState> m_pathCoordinator;

    // Rebuild coordination
    mutable std::mutex m_rebuildMutex;

    // Runtime counters
    std::atomic<int> m_processedCount{0};
    std::atomic<size_t> m_preparingCount{0};
    std::atomic<size_t> m_writingCount{0};
    std::atomic<size_t> m_failedCount{0};
    std::atomic<size_t> m_retriedCount{0};
    std::atomic<size_t> m_committedCount{0};
    std::atomic<size_t> m_coalescedCount{0};
    std::atomic<size_t> m_staleDroppedCount{0};
    std::atomic<size_t> m_writerBatchDepth{0};

    // Concurrency policy
    std::atomic<bool> m_userActive{false};
    std::atomic<size_t> m_allowedPrepWorkers{1};
    size_t m_idlePrepWorkers = 2;

    std::vector<std::string> m_scanRoots;

    static constexpr int kBatchCommitSize = 50;
    static constexpr int kBatchCommitIntervalMs = 250;
    static constexpr int kMaxPipelineRetries = 3;
    static constexpr size_t kScanHighWatermark = 8000;
    static constexpr size_t kScanResumeWatermark = 5000;
    static constexpr int kEnqueueRetrySleepMs = 25;
};

} // namespace bs
