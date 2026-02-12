#include "core/indexing/pipeline.h"
#include "core/index/sqlite_store.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/file_scanner.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QByteArray>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace bs {

namespace {

using Clock = std::chrono::steady_clock;

int readEnvInt(const char* key, int fallback, int minValue, int maxValue)
{
    const QByteArray value = qgetenv(key);
    if (value.isEmpty()) {
        return fallback;
    }

    bool ok = false;
    const int parsed = QString::fromUtf8(value).toInt(&ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(parsed, minValue, maxValue);
}

int currentProcessRssMb()
{
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t kr = task_info(mach_task_self(),
                                       MACH_TASK_BASIC_INFO,
                                       reinterpret_cast<task_info_t>(&info),
                                       &count);
    if (kr != KERN_SUCCESS) {
        return -1;
    }

    return static_cast<int>(info.resident_size / (1024 * 1024));
#else
    return -1;
#endif
}

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

Pipeline::ActorMode readActorMode()
{
    const QString raw =
        qEnvironmentVariable("BETTERSPOTLIGHT_PIPELINE_ACTOR_MODE").trimmed().toLower();
    if (raw == QLatin1String("legacy")) {
        return Pipeline::ActorMode::Legacy;
    }
    if (raw == QLatin1String("actor_primary")) {
        return Pipeline::ActorMode::ActorPrimary;
    }
    return Pipeline::ActorMode::Dual;
}

} // namespace

// ── Construction / destruction ──────────────────────────────

Pipeline::Pipeline(SQLiteStore& store, ExtractionManager& extractor,
                   const PathRules& pathRules,
                   const PipelineRuntimeConfig& runtimeConfig,
                   QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_extractor(extractor)
    , m_pathRules(pathRules)
    , m_chunker()
    , m_workQueue()
    , m_indexer(std::make_unique<Indexer>(m_store, m_extractor, m_pathRules, m_chunker))
    , m_monitor(std::make_unique<FileMonitorMacOS>())
    , m_runtimeConfig(runtimeConfig)
{
    m_runtimeConfig.batchCommitSize = std::max(1, m_runtimeConfig.batchCommitSize);
    m_runtimeConfig.batchCommitIntervalMs = std::max(1, m_runtimeConfig.batchCommitIntervalMs);
    m_runtimeConfig.maxPipelineRetries = std::max(0, m_runtimeConfig.maxPipelineRetries);
    m_runtimeConfig.scanHighWatermark = std::max<size_t>(1, m_runtimeConfig.scanHighWatermark);
    m_runtimeConfig.scanResumeWatermark = std::min(
        m_runtimeConfig.scanResumeWatermark,
        m_runtimeConfig.scanHighWatermark);
    m_runtimeConfig.enqueueRetrySleepMs = std::max(1, m_runtimeConfig.enqueueRetrySleepMs);
    m_runtimeConfig.memoryPressureSleepMs = std::max(1, m_runtimeConfig.memoryPressureSleepMs);
    m_runtimeConfig.drainPollAttempts = std::max(1, m_runtimeConfig.drainPollAttempts);
    m_runtimeConfig.drainPollIntervalMs = std::max(1, m_runtimeConfig.drainPollIntervalMs);
    m_runtimeConfig.retryBackoffBaseMs = std::max(1, m_runtimeConfig.retryBackoffBaseMs);
    m_runtimeConfig.retryBackoffCapMs = std::max(
        m_runtimeConfig.retryBackoffBaseMs,
        m_runtimeConfig.retryBackoffCapMs);

    m_idlePrepWorkers = computeIdlePrepWorkers();
    m_memoryPressurePrepWorkers = static_cast<size_t>(readEnvInt(
        "BETTERSPOTLIGHT_INDEXER_PREP_WORKERS_PRESSURE",
        1,
        1,
        static_cast<int>(std::max<size_t>(1, m_idlePrepWorkers))));
    m_memorySoftLimitMb = readEnvInt("BETTERSPOTLIGHT_INDEXER_RSS_SOFT_MB", 900, 256, 32768);
    m_memoryHardLimitMb = readEnvInt("BETTERSPOTLIGHT_INDEXER_RSS_HARD_MB", 1200, 320, 32768);
    if (m_memoryHardLimitMb <= m_memorySoftLimitMb) {
        m_memoryHardLimitMb = m_memorySoftLimitMb + 128;
    }

    m_allowedPrepWorkers.store(m_idlePrepWorkers);
    m_actorMode = readActorMode();
    m_pathStateActor = std::make_unique<PathStateActor>();
    PipelineSchedulerConfig schedulerConfig;
    schedulerConfig.liveLaneCap = 4000;
    schedulerConfig.rebuildLaneCap = 20000;
    schedulerConfig.liveDispatchRatioPct = 70;
    m_schedulerActor = std::make_unique<PipelineSchedulerActor>(schedulerConfig);
    m_telemetryActor = std::make_unique<PipelineTelemetryActor>();
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
    if (m_schedulerActor) {
        m_schedulerActor->shutdown();
    }
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
    const size_t memoryAware = applyMemoryAwarePrepWorkerLimit(allowed);
    const size_t clamped = std::max<size_t>(1, std::min(memoryAware, m_idlePrepWorkers));

    m_allowedPrepWorkers.store(clamped);
    m_extractor.setMaxConcurrent(static_cast<int>(clamped));
    if (m_telemetryActor) {
        m_telemetryActor->recordPrepWorkers(clamped);
    }

    wakeAllStages();
}

size_t Pipeline::applyMemoryAwarePrepWorkerLimit(size_t requestedWorkers) const
{
    const int rssMb = processRssMb();
    if (rssMb < 0) {
        return requestedWorkers;
    }

    if (rssMb >= m_memoryHardLimitMb) {
        return 1;
    }

    if (rssMb >= m_memorySoftLimitMb) {
        return std::min(requestedWorkers, m_memoryPressurePrepWorkers);
    }

    return requestedWorkers;
}

int Pipeline::processRssMb() const
{
    if (m_runtimeConfig.rssProvider) {
        return m_runtimeConfig.rssProvider();
    }
    return currentProcessRssMb();
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
        m_preparedLiveQueue.clear();
        m_preparedRebuildQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_coordMutex);
        m_pathCoordinator.clear();
    }
    if (m_pathStateActor) {
        m_pathStateActor->reset();
    }
    if (m_telemetryActor) {
        m_telemetryActor->reset();
    }
    m_livePending.store(0);
    m_rebuildPending.store(0);
}

void Pipeline::wakeAllStages()
{
    m_prepCv.notify_all();
    m_preparedCv.notify_all();
    if (m_schedulerActor) {
        m_schedulerActor->notifyAll();
    }
}

size_t Pipeline::pendingMergedCount() const
{
    if (m_actorMode == ActorMode::ActorPrimary && m_pathStateActor) {
        return m_pathStateActor->pendingMergedCount();
    }

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
    size_t ingress = 0;
    if (m_actorMode == ActorMode::ActorPrimary && m_schedulerActor) {
        ingress = m_schedulerActor->totalDepth();
    } else {
        ingress = m_workQueue.size();
    }

    size_t prep = 0;
    {
        std::lock_guard<std::mutex> lock(m_prepMutex);
        prep = m_prepQueue.size();
    }

    size_t preparedLive = 0;
    size_t preparedRebuild = 0;
    {
        std::lock_guard<std::mutex> lock(m_preparedMutex);
        preparedLive = m_preparedLiveQueue.size();
        preparedRebuild = m_preparedRebuildQueue.size();
    }

    return ingress + prep + preparedLive + preparedRebuild + pendingMergedCount();
}

bool Pipeline::waitForScanBackpressureWindow() const
{
    while (m_running.load() && !m_stopping.load()) {
        const size_t depth = totalPendingDepth();
        const int rssMb = processRssMb();
        const bool hardMemoryPressure = rssMb >= m_memoryHardLimitMb;
        if (depth <= m_runtimeConfig.scanHighWatermark && !hardMemoryPressure) {
            return true;
        }

        while (m_running.load() && !m_stopping.load()
               && (totalPendingDepth() > m_runtimeConfig.scanResumeWatermark
                   || processRssMb() >= m_memoryHardLimitMb)) {
            const int pressureSleepMs =
                processRssMb() >= m_memorySoftLimitMb
                    ? m_runtimeConfig.memoryPressureSleepMs
                    : m_runtimeConfig.enqueueRetrySleepMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(pressureSleepMs));
        }
    }

    return false;
}

bool Pipeline::enqueuePrimaryWorkItem(const WorkItem& item, int maxAttempts)
{
    return enqueueLaneWorkItem(item,
                               item.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live,
                               maxAttempts);
}

bool Pipeline::enqueueLaneWorkItem(const WorkItem& item, PipelineLane lane, int maxAttempts)
{
    WorkItem laneItem = item;
    laneItem.rebuildLane = (lane == PipelineLane::Rebuild);

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (!m_running.load() || m_stopping.load()) {
            return false;
        }

        if (!waitForScanBackpressureWindow()) {
            QString dropReason = QStringLiteral("writer_lag");
            const int rssMb = processRssMb();
            if (rssMb >= m_memoryHardLimitMb) {
                dropReason = QStringLiteral("memory_hard");
            } else if (rssMb >= m_memorySoftLimitMb) {
                dropReason = QStringLiteral("memory_soft");
            }
            if (m_schedulerActor) {
                m_schedulerActor->recordDrop(lane, dropReason);
            }
            if (m_telemetryActor) {
                m_telemetryActor->recordDrop(lane, dropReason);
            }
            return false;
        }

        bool enqueued = false;
        if (m_actorMode == ActorMode::ActorPrimary) {
            enqueued = m_schedulerActor && m_schedulerActor->enqueue(laneItem, lane);
        } else {
            enqueued = m_workQueue.enqueue(laneItem);
            if (m_actorMode == ActorMode::Dual && m_schedulerActor) {
                // Dual mode runs actorized ingress in shadow for drift checks.
                (void)m_schedulerActor->enqueue(laneItem, lane);
            }
        }

        if (enqueued) {
            if (lane == PipelineLane::Live) {
                m_livePending.fetch_add(1);
            } else {
                m_rebuildPending.fetch_add(1);
            }
            return true;
        }

        if (m_schedulerActor) {
            m_schedulerActor->recordDrop(lane, QStringLiteral("queue_full"));
        }
        if (m_telemetryActor) {
            m_telemetryActor->recordDrop(lane, QStringLiteral("queue_full"));
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_runtimeConfig.enqueueRetrySleepMs));
    }

    LOG_WARN(bsIndex, "Failed to enqueue primary work after retries: %s",
             laneItem.filePath.c_str());
    return false;
}

void Pipeline::waitForPipelineDrain()
{
    for (int attempt = 0; attempt < m_runtimeConfig.drainPollAttempts; ++attempt) {
        const bool drained = (totalPendingDepth() == 0)
            && (m_preparingCount.load() == 0)
            && (m_writingCount.load() == 0);
        if (drained) {
            return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_runtimeConfig.drainPollIntervalMs));
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
    item.rebuildLane = false;

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
            item.rebuildLane = true;
            if (enqueueLaneWorkItem(item, PipelineLane::Rebuild)) {
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
    QueueStats stats;
    if (m_actorMode == ActorMode::ActorPrimary) {
        if (m_schedulerActor) {
            const PipelineSchedulerStats schedulerStats = m_schedulerActor->stats();
            stats.depth = schedulerStats.liveDepth + schedulerStats.rebuildDepth;
            stats.droppedItems = schedulerStats.droppedQueueFull
                + schedulerStats.droppedMemorySoft
                + schedulerStats.droppedMemoryHard
                + schedulerStats.droppedWriterLag;
            stats.coalesced = schedulerStats.coalesced;
            stats.staleDropped = schedulerStats.staleDropped;
        }
        stats.isPaused = m_paused.load();
    } else {
        stats = m_workQueue.stats();
    }

    stats.depth = totalPendingDepth();
    stats.preparing = m_preparingCount.load();
    stats.writing = m_writingCount.load();
    stats.coalesced = std::max<size_t>(stats.coalesced, m_coalescedCount.load());
    stats.staleDropped = std::max<size_t>(stats.staleDropped, m_staleDroppedCount.load());
    stats.prepWorkers = m_allowedPrepWorkers.load();
    stats.writerBatchDepth = m_writerBatchDepth.load();
    stats.failedItems = stats.droppedItems + m_failedCount.load();
    // m_retriedCount is available for telemetry but QueueStats doesn't need it yet.
    stats.activeItems = stats.preparing + stats.writing;

    return stats;
}

QJsonObject Pipeline::telemetrySnapshot() const
{
    QJsonObject out;
    out[QStringLiteral("actorMode")] = actorModeString();
    out[QStringLiteral("livePending")] = static_cast<qint64>(m_livePending.load());
    out[QStringLiteral("rebuildPending")] = static_cast<qint64>(m_rebuildPending.load());
    if (m_schedulerActor) {
        const PipelineSchedulerStats schedulerStats = m_schedulerActor->stats();
        out[QStringLiteral("liveLaneDepth")] = static_cast<qint64>(schedulerStats.liveDepth);
        out[QStringLiteral("rebuildLaneDepth")] = static_cast<qint64>(schedulerStats.rebuildDepth);
        out[QStringLiteral("bulkheadDropQueueFull")] =
            static_cast<qint64>(schedulerStats.droppedQueueFull);
        out[QStringLiteral("bulkheadDropMemorySoft")] =
            static_cast<qint64>(schedulerStats.droppedMemorySoft);
        out[QStringLiteral("bulkheadDropMemoryHard")] =
            static_cast<qint64>(schedulerStats.droppedMemoryHard);
        out[QStringLiteral("bulkheadDropWriterLag")] =
            static_cast<qint64>(schedulerStats.droppedWriterLag);
        out[QStringLiteral("schedulerDispatchLive")] =
            static_cast<qint64>(schedulerStats.dispatchedLive);
        out[QStringLiteral("schedulerDispatchRebuild")] =
            static_cast<qint64>(schedulerStats.dispatchedRebuild);
    }
    if (m_telemetryActor) {
        const QJsonObject telemetry = m_telemetryActor->snapshot();
        for (auto it = telemetry.begin(); it != telemetry.end(); ++it) {
            out[it.key()] = it.value();
        }
    }
    return out;
}

QString Pipeline::actorModeString() const
{
    switch (m_actorMode) {
    case ActorMode::Legacy:
        return QStringLiteral("legacy");
    case ActorMode::Dual:
        return QStringLiteral("dual");
    case ActorMode::ActorPrimary:
        return QStringLiteral("actor_primary");
    }
    return QStringLiteral("dual");
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
            item.rebuildLane = true;
            if (!enqueueLaneWorkItem(item, PipelineLane::Rebuild)) {
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
    if (m_actorMode == ActorMode::ActorPrimary && m_pathStateActor) {
        const auto actorTask = m_pathStateActor->onIngress(item);
        if (!actorTask.has_value()) {
            if (m_schedulerActor) {
                m_schedulerActor->recordCoalesced();
            }
            if (m_telemetryActor) {
                m_telemetryActor->recordCoalesced();
            }
            m_coalescedCount.fetch_add(1);
            return std::nullopt;
        }

        PrepTask task;
        task.item = actorTask->item;
        task.generation = actorTask->generation;
        task.lane = actorTask->item.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live;
        return task;
    }

    std::lock_guard<std::mutex> lock(m_coordMutex);

    PathCoordinatorState& state = m_pathCoordinator[item.filePath];
    state.latestGeneration += 1;

    if (state.inPrep) {
        if (state.pendingMergedType.has_value()) {
            state.pendingMergedType = mergeWorkTypes(state.pendingMergedType.value(), item.type);
        } else {
            state.pendingMergedType = item.type;
        }
        state.pendingRebuildLane = state.pendingRebuildLane || item.rebuildLane;

        m_coalescedCount.fetch_add(1);
        LOG_DEBUG(bsIndex, "Coordinator coalesced path=%s gen=%lld",
                  item.filePath.c_str(),
                  static_cast<long long>(state.latestGeneration));
        return std::nullopt;
    }

    state.inPrep = true;
    state.pendingRebuildLane = false;

    PrepTask task;
    task.item = item;
    task.generation = state.latestGeneration;
    task.lane = item.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live;
    return task;
}

std::optional<Pipeline::PrepTask> Pipeline::onPrepCompleted(const PreparedWork& prepared)
{
    if (m_actorMode == ActorMode::ActorPrimary && m_pathStateActor) {
        const auto actorTask = m_pathStateActor->onPrepCompleted(prepared);
        if (!actorTask.has_value()) {
            return std::nullopt;
        }
        PrepTask task;
        task.item = actorTask->item;
        task.generation = actorTask->generation;
        task.lane = actorTask->item.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live;
        return task;
    }

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
        task.item.rebuildLane = state.pendingRebuildLane;
        task.generation = state.latestGeneration;
        task.lane = state.pendingRebuildLane ? PipelineLane::Rebuild : PipelineLane::Live;

        state.pendingMergedType.reset();
        state.pendingRebuildLane = false;
        state.inPrep = true;
        return task;
    }

    state.inPrep = false;
    return std::nullopt;
}

bool Pipeline::isStalePreparedWork(const PreparedWork& prepared) const
{
    if (m_actorMode == ActorMode::ActorPrimary && m_pathStateActor) {
        return m_pathStateActor->isStalePrepared(prepared);
    }

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
    size_t policyTick = 0;

    while (!m_stopping.load()) {
        if ((policyTick++ % 32) == 0) {
            updatePrepConcurrencyPolicy();
        }

        std::optional<WorkItem> ingressItem;
        if (m_actorMode == ActorMode::ActorPrimary && m_schedulerActor) {
            auto scheduled = m_schedulerActor->dequeueBlocking(m_stopping, m_paused);
            if (scheduled.has_value()) {
                ingressItem = scheduled->item;
                if (scheduled->lane == PipelineLane::Live) {
                    const size_t pending = m_livePending.load();
                    if (pending > 0) {
                        m_livePending.fetch_sub(1);
                    }
                } else {
                    const size_t pending = m_rebuildPending.load();
                    if (pending > 0) {
                        m_rebuildPending.fetch_sub(1);
                    }
                }
            }
        } else {
            auto queued = m_workQueue.dequeue();
            if (queued.has_value()) {
                ingressItem = queued.value();
                if (ingressItem->rebuildLane) {
                    const size_t pending = m_rebuildPending.load();
                    if (pending > 0) {
                        m_rebuildPending.fetch_sub(1);
                    }
                } else {
                    const size_t pending = m_livePending.load();
                    if (pending > 0) {
                        m_livePending.fetch_sub(1);
                    }
                }
                // Drain actorized scheduler in dual mode to keep shadow path bounded.
                if (m_actorMode == ActorMode::Dual && m_schedulerActor) {
                    (void)m_schedulerActor->tryDequeue();
                }
            }
        }

        if (!ingressItem.has_value()) {
            if (m_stopping.load() || !m_running.load()) {
                break;
            }
            continue;
        }

        auto prepTask = tryDispatchFromIngress(ingressItem.value());
        if (m_actorMode != ActorMode::ActorPrimary) {
            m_workQueue.markItemComplete();
        }

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
        prepared.rebuildLane = (task.lane == PipelineLane::Rebuild);
        m_preparingCount.fetch_sub(1);

        {
            std::lock_guard<std::mutex> lock(m_preparedMutex);
            if (prepared.rebuildLane) {
                m_preparedRebuildQueue.push_back(prepared);
            } else {
                m_preparedLiveQueue.push_back(prepared);
            }
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
    int writerDispatchCycle = 0;
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
        if (m_telemetryActor) {
            m_telemetryActor->recordWriterBatchDepth(0);
        }

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
                return !m_preparedLiveQueue.empty()
                    || !m_preparedRebuildQueue.empty()
                    || m_stopping.load();
            });

            if (m_preparedLiveQueue.empty() && m_preparedRebuildQueue.empty()) {
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
                const bool hasLive = !m_preparedLiveQueue.empty();
                const bool hasRebuild = !m_preparedRebuildQueue.empty();
                bool pickLive = hasLive;
                if (hasLive && hasRebuild) {
                    const int slot = writerDispatchCycle % 100;
                    pickLive = (slot < 70);
                    ++writerDispatchCycle;
                }
                if (pickLive) {
                    prepared = std::move(m_preparedLiveQueue.front());
                    m_preparedLiveQueue.pop_front();
                } else {
                    prepared = std::move(m_preparedRebuildQueue.front());
                    m_preparedRebuildQueue.pop_front();
                }
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
        const PipelineLane lane =
            prepared.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live;

        if (isStalePreparedWork(prepared)) {
            m_staleDroppedCount.fetch_add(1);
            if (m_schedulerActor) {
                m_schedulerActor->recordStaleDropped();
            }
            if (m_telemetryActor) {
                m_telemetryActor->recordStaleDrop();
            }
            LOG_DEBUG(bsIndex, "Writer dropped stale work path=%s gen=%lld",
                      qUtf8Printable(prepared.path),
                      static_cast<long long>(prepared.generation));
        } else {
            IndexResult result = m_indexer->applyPreparedWork(prepared);
            m_processedCount.fetch_add(1);

            if (result.status == IndexResult::Status::ExtractionFailed) {
                const bool shouldRetry = isTransientExtractionFailure(prepared)
                    && prepared.retryCount < m_runtimeConfig.maxPipelineRetries;
                if (shouldRetry) {
                    WorkItem retryItem;
                    retryItem.type = WorkItem::Type::ModifiedContent;
                    retryItem.filePath = prepared.path.toStdString();
                    retryItem.retryCount = prepared.retryCount + 1;

                    const int backoffMs = std::min(
                        m_runtimeConfig.retryBackoffBaseMs * (1 << (prepared.retryCount * 2)),
                        m_runtimeConfig.retryBackoffCapMs);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));

                    if (enqueueLaneWorkItem(
                            retryItem,
                            prepared.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live,
                            80)) {
                        m_retriedCount.fetch_add(1);
                        LOG_INFO(bsIndex, "Re-enqueued for retry (%d/%d): %s",
                                 retryItem.retryCount,
                                 m_runtimeConfig.maxPipelineRetries,
                                 qUtf8Printable(prepared.path));
                    } else {
                        m_failedCount.fetch_add(1);
                        LOG_WARN(bsIndex, "Failed to re-enqueue retry (%d/%d): %s",
                                 retryItem.retryCount,
                                 m_runtimeConfig.maxPipelineRetries,
                                 qUtf8Printable(prepared.path));
                    }
                } else {
                    m_failedCount.fetch_add(1);
                }
            }

            ++batchCount;
            m_writerBatchDepth.store(static_cast<size_t>(batchCount));
            if (m_telemetryActor) {
                m_telemetryActor->recordWriterDispatch(lane);
                m_telemetryActor->recordWriterBatchDepth(static_cast<size_t>(batchCount));
            }

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
            preparedQueueEmpty = m_preparedLiveQueue.empty()
                && m_preparedRebuildQueue.empty();
        }

        const bool queueDrained = prepQueueEmpty
            && m_preparingCount.load() == 0
            && ((m_actorMode == ActorMode::ActorPrimary && m_schedulerActor)
                    ? (m_schedulerActor->totalDepth() == 0)
                    : (m_workQueue.size() == 0))
            && preparedQueueEmpty
            && pendingMergedCount() == 0;

        const bool commitForSize = batchCount >= m_runtimeConfig.batchCommitSize;
        const bool commitForTime =
            batchTimer.isValid()
            && batchTimer.elapsed() >= m_runtimeConfig.batchCommitIntervalMs;

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
        const PipelineLane lane = item.rebuildLane ? PipelineLane::Rebuild : PipelineLane::Live;
        if (item.type == WorkItem::Type::NewFile
            || item.type == WorkItem::Type::ModifiedContent) {
            if (enqueueLaneWorkItem(item, lane, 80)) {
                ++enqueued;
            } else {
                m_failedCount.fetch_add(1);
            }
        } else if (enqueueLaneWorkItem(item, lane, 80)) {
            ++enqueued;
        }
    }

    if (enqueued > 0) {
        LOG_DEBUG(bsIndex, "FS events: %d received, %d enqueued",
                  static_cast<int>(items.size()), enqueued);
    }
}

} // namespace bs
