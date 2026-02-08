#include "core/indexing/pipeline.h"
#include "core/index/sqlite_store.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/file_scanner.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QElapsedTimer>

namespace bs {

// ── Construction / destruction ──────────────────────────────

Pipeline::Pipeline(SQLiteStore& store, ExtractionManager& extractor,
                   const PathRules& pathRules, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_extractor(extractor)
    , m_pathRules(pathRules)
    , m_chunker()
    , m_workQueue()
    , m_indexer(std::make_unique<Indexer>(m_store, m_extractor, m_pathRules, m_chunker))
    , m_monitor(std::make_unique<FileMonitorMacOS>())
{
    LOG_INFO(bsIndex, "Pipeline created");
}

Pipeline::~Pipeline()
{
    stop();
}

// ── Start / stop ────────────────────────────────────────────

void Pipeline::start(const std::vector<std::string>& roots)
{
    if (m_running.load()) {
        LOG_WARN(bsIndex, "Pipeline::start() called while already running");
        return;
    }

    LOG_INFO(bsIndex, "Pipeline starting with %d root(s)", static_cast<int>(roots.size()));

    m_running.store(true);
    m_processedCount = 0;
    m_scanRoots = roots;

    // Start file monitoring first so FS events during scan are captured.
    bool monitorOk = m_monitor->start(roots,
        [this](const std::vector<WorkItem>& items) {
            onFileSystemEvents(items);
        });

    if (!monitorOk) {
        LOG_ERROR(bsIndex, "Failed to start file monitor");
        emit indexingError(QStringLiteral("Failed to start file monitor"));
    }

    // Start the processing thread FIRST so items are processed as they arrive.
    m_processThread = std::make_unique<QThread>();
    QObject::connect(m_processThread.get(), &QThread::started, [this] {
        processingLoop();
    });
    m_processThread->start();

    // Start the scan thread — enqueues items progressively while processing
    // runs concurrently. Runs on a separate thread so the main event loop
    // stays responsive for IPC heartbeats during the initial directory walk.
    m_workerThread = std::make_unique<QThread>();
    QObject::connect(m_workerThread.get(), &QThread::started, [this] {
        scanEntry();
    });
    m_workerThread->start();

    LOG_INFO(bsIndex, "Pipeline started (scan and processing on separate threads)");
}

void Pipeline::stop()
{
    if (!m_running.load()) {
        return;
    }

    LOG_INFO(bsIndex, "Pipeline stopping...");

    m_running.store(false);

    // Stop the file monitor first (no new events)
    if (m_monitor) {
        m_monitor->stop();
    }

    // Signal the work queue to unblock the processing loop's dequeue() wait
    m_workQueue.shutdown();

    // Wait for the processing thread to finish (unblocked by shutdown above)
    if (m_processThread && m_processThread->isRunning()) {
        m_processThread->quit();
        m_processThread->wait();
    }
    m_processThread.reset();

    // Wait for the scan thread to finish
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait();
    }
    m_workerThread.reset();

    LOG_INFO(bsIndex, "Pipeline stopped (processed %d items)", m_processedCount);
}

// ── Pause / resume ──────────────────────────────────────────

void Pipeline::pause()
{
    m_workQueue.pause();
    LOG_INFO(bsIndex, "Pipeline paused");
}

void Pipeline::resume()
{
    m_workQueue.resume();
    LOG_INFO(bsIndex, "Pipeline resumed");
}

// ── Re-index / rebuild ──────────────────────────────────────

void Pipeline::reindexPath(const QString& path)
{
    WorkItem item;
    item.type = WorkItem::Type::ModifiedContent;
    item.filePath = path.toStdString();

    LOG_INFO(bsIndex, "Re-index requested: %s", qUtf8Printable(path));
    m_workQueue.enqueue(std::move(item));
}

void Pipeline::rebuildAll(const std::vector<std::string>& roots)
{
    LOG_INFO(bsIndex, "Rebuild all requested — stopping current processing");

    // Pause while we clear the store
    m_workQueue.pause();

    if (!m_store.deleteAll()) {
        LOG_ERROR(bsIndex, "rebuildAll: failed to clear index; aborting rebuild");
        m_workQueue.resume();
        emit indexingError(QStringLiteral("Failed to clear index for rebuild"));
        return;
    }

    m_processedCount = 0;

    // Re-scan all roots
    FileScanner scanner(&m_pathRules);
    for (const auto& root : roots) {
        auto files = scanner.scanDirectory(root);
        for (auto& meta : files) {
            WorkItem item;
            item.type = WorkItem::Type::NewFile;
            item.filePath = std::move(meta.filePath);
            m_workQueue.enqueue(std::move(item));
        }
    }

    m_workQueue.resume();
    LOG_INFO(bsIndex, "Rebuild all: queued %d items", static_cast<int>(m_workQueue.size()));
}

QueueStats Pipeline::queueStatus() const
{
    return m_workQueue.stats();
}

// ── Scan thread entry ────────────────────────────────────────

void Pipeline::scanEntry()
{
    FileScanner scanner(&m_pathRules);
    for (const auto& root : m_scanRoots) {
        if (!m_running.load()) break;

        LOG_INFO(bsIndex, "Initial scan: %s", root.c_str());
        auto files = scanner.scanDirectory(root);
        int totalFiles = static_cast<int>(files.size());
        LOG_INFO(bsIndex, "Initial scan found %d files in %s", totalFiles, root.c_str());

        for (auto& meta : files) {
            WorkItem item;
            item.type = WorkItem::Type::NewFile;
            item.filePath = std::move(meta.filePath);
            m_workQueue.enqueue(std::move(item));
        }
    }

    LOG_INFO(bsIndex, "Initial scan complete, queue depth: %d",
             static_cast<int>(m_workQueue.size()));
}

// ── Processing loop (runs on process thread) ─────────────────

void Pipeline::processingLoop()
{
    LOG_INFO(bsIndex, "Processing loop started on process thread");

    int batchCount = 0;
    bool inTransaction = false;

    while (m_running.load()) {
        auto item = m_workQueue.dequeue();
        if (!item.has_value()) {
            // Paused or shutting down
            if (inTransaction) {
                m_store.commitTransaction();
                inTransaction = false;
            }
            if (!m_running.load()) {
                break;
            }
            // Paused — loop back and wait for resume/shutdown
            continue;
        }

        // Begin a transaction batch if not already in one
        if (!inTransaction) {
            m_store.beginTransaction();
            inTransaction = true;
            batchCount = 0;
        }

        // Process the work item
        IndexResult result = m_indexer->processWorkItem(item.value());

        ++m_processedCount;
        ++batchCount;

        LOG_DEBUG(bsIndex, "Processed %s: status=%d, chunks=%d, duration=%d ms",
                  item->filePath.c_str(),
                  static_cast<int>(result.status),
                  result.chunksInserted,
                  result.durationMs);

        // Throttle CPU: sleep between items to stay < 50% of one core
        int sleepMs = m_userActive.load() ? kThrottleActiveMs : kThrottleIdleMs;
        QThread::msleep(sleepMs);

        // Commit the batch at the threshold
        if (batchCount >= kBatchCommitSize) {
            m_store.commitTransaction();
            inTransaction = false;
            batchCount = 0;

            emit progressUpdated(m_processedCount,
                                 m_processedCount + static_cast<int>(m_workQueue.size()));
        }
    }

    // Commit any remaining items
    if (inTransaction) {
        m_store.commitTransaction();
    }

    // Record completion timestamp for index age calculation
    m_store.setSetting(QStringLiteral("last_full_index_at"),
                       QString::number(QDateTime::currentSecsSinceEpoch()));

    LOG_INFO(bsIndex, "Processing loop exiting (total processed: %d)", m_processedCount);
    emit indexingComplete();
}

// ── FS event callback ───────────────────────────────────────

void Pipeline::onFileSystemEvents(const std::vector<WorkItem>& items)
{
    int enqueued = 0;
    for (const auto& item : items) {
        // Filter FS events through PathRules to avoid queueing work
        // for excluded paths (e.g., Library/Application Support/AddressBook).
        auto validation = m_pathRules.validate(item.filePath);
        if (validation == ValidationResult::Exclude) {
            continue;
        }
        m_workQueue.enqueue(item);
        ++enqueued;
    }

    if (enqueued > 0) {
        LOG_DEBUG(bsIndex, "FS events: %d received, %d enqueued",
                  static_cast<int>(items.size()), enqueued);
    }
}

} // namespace bs
