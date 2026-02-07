#pragma once

#include "core/indexing/chunker.h"
#include "core/indexing/indexer.h"
#include "core/indexing/work_queue.h"
#include "core/fs/file_monitor_macos.h"
#include "core/fs/path_rules.h"

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace bs {

class SQLiteStore;
class ExtractionManager;

// Pipeline — top-level indexing orchestrator.
//
// Owns the WorkQueue, Chunker, and Indexer. Wires them to the FileMonitor
// for live FS event processing and to SQLiteStore + ExtractionManager for
// content pipeline stages. Runs the processing loop on a dedicated QThread.
//
// Transaction batching: commits every kBatchCommitSize items to balance
// throughput vs. write amplification.
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

    // Stop monitoring and processing. Blocks until the worker thread exits.
    void stop();

    // Pause/resume the work queue (processing loop sleeps while paused).
    void pause();
    void resume();

    // Enqueue a single path for re-indexing.
    void reindexPath(const QString& path);

    // Drop all indexed data and re-scan from scratch.
    void rebuildAll(const std::vector<std::string>& roots);

    // Hint that the user is actively interacting (e.g. typing a query).
    // When active, the processing loop throttles harder to keep CPU < 50%.
    void setUserActive(bool active) { m_userActive.store(active); }

    // Snapshot of current queue statistics.
    QueueStats queueStatus() const;

    // Number of items processed so far.
    int processedCount() const { return m_processedCount; }

signals:
    void progressUpdated(int processedCount, int totalCount);
    void indexingComplete();
    void indexingError(const QString& error);

private:
    // Scan thread entry point: walks directories and enqueues work items.
    void scanEntry();
    // Processing thread entry point: dequeues and processes work items.
    void processingLoop();

    // Callback from FileMonitorMacOS — enqueues incoming FS events.
    void onFileSystemEvents(const std::vector<WorkItem>& items);

    SQLiteStore& m_store;
    ExtractionManager& m_extractor;
    PathRules m_pathRules;
    Chunker m_chunker;
    WorkQueue m_workQueue;
    std::unique_ptr<Indexer> m_indexer;
    std::unique_ptr<FileMonitorMacOS> m_monitor;
    std::unique_ptr<QThread> m_workerThread;
    std::unique_ptr<QThread> m_processThread;
    std::atomic<bool> m_running{false};
    std::vector<std::string> m_scanRoots;
    int m_processedCount = 0;

    static constexpr int kBatchCommitSize = 100;
    static constexpr int kThrottleIdleMs = 5;    // sleep between items when idle
    static constexpr int kThrottleActiveMs = 50;  // sleep between items when user is active
    std::atomic<bool> m_userActive{false};
};

} // namespace bs
