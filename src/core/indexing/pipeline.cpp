#include "core/indexing/pipeline.h"
#include "core/index/sqlite_store.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/file_scanner.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QElapsedTimer>

#include <algorithm>
#include <chrono>

namespace bs {

namespace {

using Clock = std::chrono::steady_clock;

bool isTransientExtractionFailure(const PreparedWork& prepared)
{
    if (!prepared.failure.has_value()) {
        return false;
    }
    if (prepared.failure->stage != QStringLiteral("extraction")) {
        return false;
    }

    const auto status = prepared.failure->extractionStatus.value_or(ExtractionResult::Status::Unknown);
    switch (status) {
    case ExtractionResult::Status::Inaccessible:
    case ExtractionResult::Status::Timeout:
    case ExtractionResult::Status::Unknown:
        return true;
    case ExtractionResult::Status::Success:
    case ExtractionResult::Status::CorruptedFile:
    case ExtractionResult::Status::UnsupportedFormat:
    case ExtractionResult::Status::SizeExceeded:
    case ExtractionResult::Status::Cancelled:
        return false;
    }

    return false;
}

} // namespace

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
    m_idlePrepWorkers = computeIdlePrepWorkers();
    m_allowedPrepWorkers.store(m_idlePrepWorkers);
    LOG_INFO(bsIndex, "Pipeline created (idle prep workers=%d)",
             static_cast<int>(m_idlePrepWorkers));
}

Pipeline::~Pipeline()
{
    stop();
}

// ── Lifecycle ───────────────────────────────────────────────

void Pipeline::start(const std::vector<std::string>& roots)
{
    if (m_running.load()) {
        LOG_WARN(bsIndex, "Pipeline::start() called while already running");
        return;
    }

    LOG_INFO(bsIndex, "Pipeline starting with %d root(s)", static_cast<int>(roots.size()));

    resetRuntimeState();

    m_scanRoots = roots;
    m_running.store(true);
    m_stopping.store(false);
    m_paused.store(false);

    updatePrepConcurrencyPolicy();

    bool monitorOk = m_monitor->start(roots,
        [this](const std::vector<WorkItem>& items) {
            onFileSystemEvents(items);
        });

    if (!monitorOk) {
        LOG_ERROR(bsIndex, "Failed to start file monitor");
        emit indexingError(QStringLiteral("Failed to start file monitor"));
    }

    m_scanThread = std::thread([this] { scanEntry(); });
    m_dispatchThread = std::thread([this] { prepDispatcherLoop(); });

    m_prepThreads.clear();
    m_prepThreads.reserve(m_idlePrepWorkers);
    for (size_t i = 0; i < m_idlePrepWorkers; ++i) {
        m_prepThreads.emplace_back([this, i] { prepWorkerLoop(i); });
    }

    m_writerThread = std::thread([this] { writerLoop(); });

    LOG_INFO(bsIndex, "Pipeline started (dispatcher + %d prep workers + writer)",
             static_cast<int>(m_idlePrepWorkers));
}

void Pipeline::stop()
{
    if (!m_running.load() && !m_stopping.load()) {
        return;
    }

    LOG_INFO(bsIndex, "Pipeline stopping...");

    m_stopping.store(true);
    m_running.store(false);

    if (m_monitor) {
        m_monitor->stop();
    }

    m_workQueue.shutdown();
    wakeAllStages();

    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }
    if (m_dispatchThread.joinable()) {
        m_dispatchThread.join();
    }

    for (auto& worker : m_prepThreads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_prepThreads.clear();

    if (m_writerThread.joinable()) {
        m_writerThread.join();
    }

    LOG_INFO(bsIndex, "Pipeline stopped (processed %d items)",
             m_processedCount.load());
}

// ── Pause / resume ──────────────────────────────────────────

void Pipeline::pause()
{
    m_paused.store(true);
    m_workQueue.pause();
    wakeAllStages();
    LOG_INFO(bsIndex, "Pipeline paused");
}

void Pipeline::resume()
{
    m_paused.store(false);
    m_workQueue.resume();
    wakeAllStages();
    LOG_INFO(bsIndex, "Pipeline resumed");
}

// ── Concurrency policy ──────────────────────────────────────

void Pipeline::setUserActive(bool active)
{
    const bool previous = m_userActive.exchange(active);
    if (previous == active) {
        return;
    }

    updatePrepConcurrencyPolicy();
    LOG_INFO(bsIndex, "Pipeline user activity changed: active=%d", active ? 1 : 0);
}

void Pipeline::updatePrepConcurrencyPolicy()
{
    const size_t allowed = m_userActive.load() ? 1 : m_idlePrepWorkers;
    const size_t clamped = std::max<size_t>(1, std::min(allowed, m_idlePrepWorkers));

    m_allowedPrepWorkers.store(clamped);
    m_extractor.setMaxConcurrent(static_cast<int>(clamped));

    wakeAllStages();
}

size_t Pipeline::computeIdlePrepWorkers()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        return 2;
    }

    const size_t quarter = static_cast<size_t>(hw / 4);
    return std::clamp<size_t>(quarter, 2, 3);
}

// ── Runtime helpers ─────────────────────────────────────────

void Pipeline::resetRuntimeState()
{
    m_processedCount.store(0);
    m_preparingCount.store(0);
    m_writingCount.store(0);
    m_failedCount.store(0);
    m_retriedCount.store(0);
    m_committedCount.store(0);
    m_coalescedCount.store(0);
    m_staleDroppedCount.store(0);
    m_writerBatchDepth.store(0);

    {
        std::lock_guard<std::mutex> lock(m_prepMutex);
        m_prepQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_preparedMutex);
        m_preparedQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_coordMutex);
        m_pathCoordinator.clear();
    }
}

void Pipeline::wakeAllStages()
{
    m_prepCv.notify_all();
    m_preparedCv.notify_all();
}

size_t Pipeline::pendingMergedCount() const
{
    std::lock_guard<std::mutex> lock(m_coordMutex);
    size_t count = 0;
    for (const auto& [_, state] : m_pathCoordinator) {
        if (state.pendingMergedType.has_value()) {
            ++count;
        }
    }
    return count;
}

size_t Pipeline::totalPendingDepth() const
{
    size_t ingress = m_workQueue.size();

    size_t prep = 0;
    {
        std::lock_guard<std::mutex> lock(m_prepMutex);
        prep = m_prepQueue.size();
    }

    size_t prepared = 0;
    {
        std::lock_guard<std::mutex> lock(m_preparedMutex);
        prepared = m_preparedQueue.size();
    }

    return ingress + prep + prepared + pendingMergedCount();
}

bool Pipeline::waitForScanBackpressureWindow() const
{
    while (m_running.load() && !m_stopping.load()) {
        if (totalPendingDepth() <= kScanHighWatermark) {
            return true;
        }

        while (m_running.load() && !m_stopping.load()
               && totalPendingDepth() > kScanResumeWatermark) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kEnqueueRetrySleepMs));
        }
    }

    return false;
}

bool Pipeline::enqueuePrimaryWorkItem(const WorkItem& item, int maxAttempts)
{
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (!m_running.load() || m_stopping.load()) {
            return false;
        }

        if (!waitForScanBackpressureWindow()) {
            return false;
        }

        if (m_workQueue.enqueue(item)) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kEnqueueRetrySleepMs));
    }

    LOG_WARN(bsIndex, "Failed to enqueue primary work after retries: %s",
             item.filePath.c_str());
    return false;
}

void Pipeline::waitForPipelineDrain()
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        const bool drained = (totalPendingDepth() == 0)
            && (m_preparingCount.load() == 0)
            && (m_writingCount.load() == 0);
        if (drained) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    LOG_WARN(bsIndex, "waitForPipelineDrain timed out; continuing rebuild with residual activity");
}

WorkItem::Type Pipeline::mergeWorkTypes(WorkItem::Type lhs, WorkItem::Type rhs)
{
    auto rank = [](WorkItem::Type t) -> int {
        switch (t) {
        case WorkItem::Type::Delete:
            return 0;
        case WorkItem::Type::ModifiedContent:
            return 1;
        case WorkItem::Type::NewFile:
            return 2;
        case WorkItem::Type::RescanDirectory:
            return 3;
        }
        return 3;
    };

    return (rank(lhs) <= rank(rhs)) ? lhs : rhs;
}

// ── Re-index / rebuild ──────────────────────────────────────

void Pipeline::reindexPath(const QString& path)
{
    WorkItem item;
    item.type = WorkItem::Type::ModifiedContent;
    item.filePath = path.toStdString();

    LOG_INFO(bsIndex, "Re-index requested: %s", qUtf8Printable(path));
    if (!enqueuePrimaryWorkItem(item, 200)) {
        m_failedCount.fetch_add(1);
        LOG_WARN(bsIndex, "Re-index request dropped after retries: %s",
                 qUtf8Printable(path));
    }
}

void Pipeline::rebuildAll(const std::vector<std::string>& roots)
{
    std::lock_guard<std::mutex> rebuildLock(m_rebuildMutex);

    if (!m_running.load()) {
        LOG_WARN(bsIndex, "rebuildAll called while pipeline is not running");
        return;
    }

    LOG_INFO(bsIndex, "Rebuild all requested");

    pause();
    waitForPipelineDrain();

    if (!m_store.deleteAll()) {
        LOG_ERROR(bsIndex, "rebuildAll: failed to clear index; aborting rebuild");
        resume();
        emit indexingError(QStringLiteral("Failed to clear index for rebuild"));
        return;
    }

    m_processedCount.store(0);
    {
        std::lock_guard<std::mutex> lock(m_coordMutex);
        m_pathCoordinator.clear();
    }

    FileScanner scanner(&m_pathRules);
    size_t enqueued = 0;
    for (const auto& root : roots) {
        auto files = scanner.scanDirectory(root);
        for (auto& meta : files) {
            WorkItem item;
            item.type = WorkItem::Type::NewFile;
            item.filePath = std::move(meta.filePath);
            if (enqueuePrimaryWorkItem(item)) {
                ++enqueued;
            } else {
                m_failedCount.fetch_add(1);
            }
        }
    }

    resume();

    LOG_INFO(bsIndex, "Rebuild all: queued %d items", static_cast<int>(enqueued));
}

QueueStats Pipeline::queueStatus() const
{
    QueueStats stats = m_workQueue.stats();

    stats.depth = totalPendingDepth();
    stats.preparing = m_preparingCount.load();
    stats.writing = m_writingCount.load();
    stats.coalesced = m_coalescedCount.load();
    stats.staleDropped = m_staleDroppedCount.load();
    stats.prepWorkers = m_allowedPrepWorkers.load();
    stats.writerBatchDepth = m_writerBatchDepth.load();
    stats.failedItems = stats.droppedItems + m_failedCount.load();
    // m_retriedCount is available for telemetry but QueueStats doesn't need it yet.
    stats.activeItems = stats.preparing + stats.writing;

    return stats;
}

// ── Scan thread ─────────────────────────────────────────────

void Pipeline::scanEntry()
{
    FileScanner scanner(&m_pathRules);

    for (const auto& root : m_scanRoots) {
        if (!m_running.load() || m_stopping.load()) {
            break;
        }

        LOG_INFO(bsIndex, "Initial scan: %s", root.c_str());
        auto files = scanner.scanDirectory(root);
        LOG_INFO(bsIndex, "Initial scan found %d files in %s",
                 static_cast<int>(files.size()), root.c_str());

        for (auto& meta : files) {
            if (!m_running.load() || m_stopping.load()) {
                break;
            }

            WorkItem item;
            item.type = WorkItem::Type::NewFile;
            item.filePath = std::move(meta.filePath);
            if (!enqueuePrimaryWorkItem(item)) {
                m_failedCount.fetch_add(1);
            }
        }
    }

    LOG_INFO(bsIndex, "Initial scan complete, queue depth: %d",
             static_cast<int>(m_workQueue.size()));
}

// ── Coordinator helpers ─────────────────────────────────────

std::optional<Pipeline::PrepTask> Pipeline::tryDispatchFromIngress(const WorkItem& item)
{
    std::lock_guard<std::mutex> lock(m_coordMutex);

    PathCoordinatorState& state = m_pathCoordinator[item.filePath];
    state.latestGeneration += 1;

    if (state.inPrep) {
        if (state.pendingMergedType.has_value()) {
            state.pendingMergedType = mergeWorkTypes(state.pendingMergedType.value(), item.type);
        } else {
            state.pendingMergedType = item.type;
        }

        m_coalescedCount.fetch_add(1);
        LOG_DEBUG(bsIndex, "Coordinator coalesced path=%s gen=%lld",
                  item.filePath.c_str(),
                  static_cast<long long>(state.latestGeneration));
        return std::nullopt;
    }

    state.inPrep = true;

    PrepTask task;
    task.item = item;
    task.generation = state.latestGeneration;
    return task;
}

std::optional<Pipeline::PrepTask> Pipeline::onPrepCompleted(const PreparedWork& prepared)
{
    const std::string path = prepared.path.toStdString();

    std::lock_guard<std::mutex> lock(m_coordMutex);
    auto it = m_pathCoordinator.find(path);
    if (it == m_pathCoordinator.end()) {
        return std::nullopt;
    }

    PathCoordinatorState& state = it->second;
    if (state.pendingMergedType.has_value()) {
        PrepTask task;
        task.item.type = state.pendingMergedType.value();
        task.item.filePath = path;
        task.generation = state.latestGeneration;

        state.pendingMergedType.reset();
        state.inPrep = true;
        return task;
    }

    state.inPrep = false;
    return std::nullopt;
}

bool Pipeline::isStalePreparedWork(const PreparedWork& prepared) const
{
    const std::string path = prepared.path.toStdString();

    std::lock_guard<std::mutex> lock(m_coordMutex);
    auto it = m_pathCoordinator.find(path);
    if (it == m_pathCoordinator.end()) {
        return false;
    }

    return prepared.generation < it->second.latestGeneration;
}

// ── Stage loops ─────────────────────────────────────────────

void Pipeline::prepDispatcherLoop()
{
    LOG_INFO(bsIndex, "Prep dispatcher loop started");

    while (!m_stopping.load()) {
        auto item = m_workQueue.dequeue();
        if (!item.has_value()) {
            if (m_stopping.load() || !m_running.load()) {
                break;
            }
            continue;
        }

        auto prepTask = tryDispatchFromIngress(item.value());
        m_workQueue.markItemComplete();

        if (!prepTask.has_value()) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_prepMutex);
            m_prepQueue.push_back(std::move(prepTask.value()));
        }
        m_prepCv.notify_one();
    }

    LOG_INFO(bsIndex, "Prep dispatcher loop exiting");
}

void Pipeline::prepWorkerLoop(size_t workerIndex)
{
    LOG_INFO(bsIndex, "Prep worker %d started", static_cast<int>(workerIndex));

    while (true) {
        PrepTask task;

        {
            std::unique_lock<std::mutex> lock(m_prepMutex);
            m_prepCv.wait(lock, [this, workerIndex] {
                if (m_stopping.load()) {
                    return true;
                }
                if (m_paused.load()) {
                    return false;
                }
                if (workerIndex >= m_allowedPrepWorkers.load()) {
                    return false;
                }
                return !m_prepQueue.empty();
            });

            if (m_stopping.load() && m_prepQueue.empty()) {
                break;
            }
            if (m_paused.load() || workerIndex >= m_allowedPrepWorkers.load() || m_prepQueue.empty()) {
                continue;
            }

            task = std::move(m_prepQueue.front());
            m_prepQueue.pop_front();
        }

        m_preparingCount.fetch_add(1);
        PreparedWork prepared = m_indexer->prepareWorkItem(task.item, task.generation);
        m_preparingCount.fetch_sub(1);

        {
            std::lock_guard<std::mutex> lock(m_preparedMutex);
            m_preparedQueue.push_back(prepared);
        }
        m_preparedCv.notify_one();

        auto nextTask = onPrepCompleted(prepared);
        if (nextTask.has_value()) {
            std::lock_guard<std::mutex> lock(m_prepMutex);
            m_prepQueue.push_back(std::move(nextTask.value()));
            m_prepCv.notify_one();
        }
    }

    LOG_INFO(bsIndex, "Prep worker %d exiting", static_cast<int>(workerIndex));
}

void Pipeline::writerLoop()
{
    LOG_INFO(bsIndex, "Writer loop started");

    bool inTransaction = false;
    int batchCount = 0;
    QElapsedTimer batchTimer;

    auto commitBatch = [this, &inTransaction, &batchCount]() {
        if (!inTransaction) {
            return;
        }

        m_store.commitTransaction();
        inTransaction = false;

        m_committedCount.fetch_add(static_cast<size_t>(batchCount));
        batchCount = 0;
        m_writerBatchDepth.store(0);

        const int processed = m_processedCount.load();
        const int total = processed + static_cast<int>(totalPendingDepth());
        emit progressUpdated(processed, total);
    };

    while (true) {
        PreparedWork prepared;
        bool shouldCommitIdleBatch = false;
        bool shouldBreak = false;
        bool hasPreparedWork = false;

        {
            std::unique_lock<std::mutex> lock(m_preparedMutex);
            m_preparedCv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return !m_preparedQueue.empty() || m_stopping.load();
            });

            if (m_preparedQueue.empty()) {
                bool prepQueueEmpty = false;
                {
                    std::lock_guard<std::mutex> prepLock(m_prepMutex);
                    prepQueueEmpty = m_prepQueue.empty();
                }

                if (m_stopping.load() && prepQueueEmpty && m_preparingCount.load() == 0) {
                    shouldBreak = true;
                } else if (inTransaction && batchCount > 0) {
                    // Commit after dropping m_preparedMutex to avoid self-deadlock
                    // through totalPendingDepth() lock acquisition.
                    shouldCommitIdleBatch = true;
                }
            } else {
                prepared = std::move(m_preparedQueue.front());
                m_preparedQueue.pop_front();
                hasPreparedWork = true;
            }
        }

        if (shouldBreak) {
            break;
        }
        if (!hasPreparedWork) {
            if (shouldCommitIdleBatch) {
                commitBatch();
            }
            continue;
        }

        if (!inTransaction) {
            m_store.beginTransaction();
            inTransaction = true;
            batchCount = 0;
            batchTimer.start();
        }

        m_writingCount.store(1);

        if (isStalePreparedWork(prepared)) {
            m_staleDroppedCount.fetch_add(1);
            LOG_DEBUG(bsIndex, "Writer dropped stale work path=%s gen=%lld",
                      qUtf8Printable(prepared.path),
                      static_cast<long long>(prepared.generation));
        } else {
            IndexResult result = m_indexer->applyPreparedWork(prepared);
            m_processedCount.fetch_add(1);

            if (result.status == IndexResult::Status::ExtractionFailed) {
                const bool shouldRetry = isTransientExtractionFailure(prepared)
                    && prepared.retryCount < kMaxPipelineRetries;
                if (shouldRetry) {
                    WorkItem retryItem;
                    retryItem.type = WorkItem::Type::ModifiedContent;
                    retryItem.filePath = prepared.path.toStdString();
                    retryItem.retryCount = prepared.retryCount + 1;

                    const int backoffMs = std::min(
                        500 * (1 << (prepared.retryCount * 2)),
                        8000);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));

                    if (m_workQueue.enqueue(retryItem)) {
                        m_retriedCount.fetch_add(1);
                        LOG_INFO(bsIndex, "Re-enqueued for retry (%d/%d): %s",
                                 retryItem.retryCount,
                                 kMaxPipelineRetries,
                                 qUtf8Printable(prepared.path));
                    } else {
                        m_failedCount.fetch_add(1);
                        LOG_WARN(bsIndex, "Failed to re-enqueue retry (%d/%d): %s",
                                 retryItem.retryCount,
                                 kMaxPipelineRetries,
                                 qUtf8Printable(prepared.path));
                    }
                } else {
                    m_failedCount.fetch_add(1);
                }
            }

            ++batchCount;
            m_writerBatchDepth.store(static_cast<size_t>(batchCount));

            LOG_DEBUG(bsIndex,
                      "Writer applied path=%s gen=%lld status=%d prep=%dms write=%dms",
                      qUtf8Printable(prepared.path),
                      static_cast<long long>(prepared.generation),
                      static_cast<int>(result.status),
                      prepared.prepDurationMs,
                      result.durationMs);
        }

        m_writingCount.store(0);

        bool prepQueueEmpty = false;
        {
            std::lock_guard<std::mutex> prepLock(m_prepMutex);
            prepQueueEmpty = m_prepQueue.empty();
        }

        bool preparedQueueEmpty = false;
        {
            std::lock_guard<std::mutex> preparedLock(m_preparedMutex);
            preparedQueueEmpty = m_preparedQueue.empty();
        }

        const bool queueDrained = prepQueueEmpty
            && m_preparingCount.load() == 0
            && m_workQueue.size() == 0
            && preparedQueueEmpty
            && pendingMergedCount() == 0;

        const bool commitForSize = batchCount >= kBatchCommitSize;
        const bool commitForTime = batchTimer.isValid() && batchTimer.elapsed() >= kBatchCommitIntervalMs;

        if (commitForSize || commitForTime || queueDrained) {
            commitBatch();
        }
    }

    if (inTransaction) {
        m_store.commitTransaction();
    }

    m_store.setSetting(QStringLiteral("last_full_index_at"),
                       QString::number(QDateTime::currentSecsSinceEpoch()));

    LOG_INFO(bsIndex, "Writer loop exiting (processed=%d committed=%d failed=%d staleDropped=%d)",
             m_processedCount.load(),
             static_cast<int>(m_committedCount.load()),
             static_cast<int>(m_failedCount.load()),
             static_cast<int>(m_staleDroppedCount.load()));

    emit indexingComplete();
}

// ── FS event callback ───────────────────────────────────────

void Pipeline::onFileSystemEvents(const std::vector<WorkItem>& items)
{
    int enqueued = 0;
    for (const auto& item : items) {
        auto validation = m_pathRules.validate(item.filePath);
        if (validation == ValidationResult::Exclude) {
            continue;
        }
        if (item.type == WorkItem::Type::NewFile
            || item.type == WorkItem::Type::ModifiedContent) {
            if (enqueuePrimaryWorkItem(item, 80)) {
                ++enqueued;
            } else {
                m_failedCount.fetch_add(1);
            }
        } else if (m_workQueue.enqueue(item)) {
            ++enqueued;
        }
    }

    if (enqueued > 0) {
        LOG_DEBUG(bsIndex, "FS events: %d received, %d enqueued",
                  static_cast<int>(items.size()), enqueued);
    }
}

} // namespace bs
