#include "core/indexing/pipeline_scheduler_actor.h"

#include <algorithm>

namespace bs {

namespace {

bool isTruthy(const QString& reason, const QString& key)
{
    return reason.compare(key, Qt::CaseInsensitive) == 0;
}

} // namespace

PipelineSchedulerActor::PipelineSchedulerActor(const PipelineSchedulerConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    if (m_config.liveLaneCap == 0) {
        m_config.liveLaneCap = 4000;
    }
    if (m_config.rebuildLaneCap == 0) {
        m_config.rebuildLaneCap = 20000;
    }
    m_config.liveDispatchRatioPct = std::clamp(m_config.liveDispatchRatioPct, 1, 99);
}

PipelineSchedulerActor::~PipelineSchedulerActor()
{
    shutdown();
}

bool PipelineSchedulerActor::enqueue(const WorkItem& item, PipelineLane lane)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        return false;
    }

    auto& queue = (lane == PipelineLane::Live) ? m_liveQueue : m_rebuildQueue;
    const size_t cap = (lane == PipelineLane::Live) ? m_config.liveLaneCap : m_config.rebuildLaneCap;
    if (queue.size() >= cap) {
        if (lane == PipelineLane::Live) {
            ++m_droppedLive;
        } else {
            ++m_droppedRebuild;
        }
        ++m_droppedQueueFull;
        return false;
    }

    queue.push_back(item);
    m_cv.notify_one();
    return true;
}

std::optional<PipelineSchedulerActor::ScheduledItem> PipelineSchedulerActor::dequeueBlocking(
    std::atomic<bool>& stopping,
    std::atomic<bool>& paused)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&]() {
        return m_shutdown
            || stopping.load()
            || (!paused.load() && (!m_liveQueue.empty() || !m_rebuildQueue.empty()));
    });

    if (m_shutdown || stopping.load() || paused.load()) {
        return std::nullopt;
    }
    return popNextLocked();
}

std::optional<PipelineSchedulerActor::ScheduledItem> PipelineSchedulerActor::tryDequeue()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        return std::nullopt;
    }
    return popNextLocked();
}

std::optional<PipelineSchedulerActor::ScheduledItem> PipelineSchedulerActor::popNextLocked()
{
    if (m_liveQueue.empty() && m_rebuildQueue.empty()) {
        return std::nullopt;
    }

    PipelineLane lane = PipelineLane::Live;
    if (m_liveQueue.empty()) {
        lane = PipelineLane::Rebuild;
    } else if (m_rebuildQueue.empty()) {
        lane = PipelineLane::Live;
    } else {
        const int slot = m_dispatchCycle % 100;
        lane = (slot < m_config.liveDispatchRatioPct) ? PipelineLane::Live : PipelineLane::Rebuild;
        ++m_dispatchCycle;
    }

    ScheduledItem out;
    out.lane = lane;
    if (lane == PipelineLane::Live) {
        out.item = std::move(m_liveQueue.front());
        m_liveQueue.pop_front();
        ++m_dispatchedLive;
    } else {
        out.item = std::move(m_rebuildQueue.front());
        m_rebuildQueue.pop_front();
        ++m_dispatchedRebuild;
    }
    return out;
}

void PipelineSchedulerActor::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_shutdown) {
        m_shutdown = true;
        m_cv.notify_all();
    }
}

void PipelineSchedulerActor::notifyAll()
{
    m_cv.notify_all();
}

void PipelineSchedulerActor::recordDrop(PipelineLane lane, const QString& reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (lane == PipelineLane::Live) {
        ++m_droppedLive;
    } else {
        ++m_droppedRebuild;
    }

    if (isTruthy(reason, QStringLiteral("memory_soft"))) {
        ++m_droppedMemorySoft;
    } else if (isTruthy(reason, QStringLiteral("memory_hard"))) {
        ++m_droppedMemoryHard;
    } else if (isTruthy(reason, QStringLiteral("writer_lag"))) {
        ++m_droppedWriterLag;
    } else {
        ++m_droppedQueueFull;
    }
}

void PipelineSchedulerActor::recordCoalesced()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_coalesced;
}

void PipelineSchedulerActor::recordStaleDropped()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_staleDropped;
}

PipelineSchedulerStats PipelineSchedulerActor::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    PipelineSchedulerStats out;
    out.liveDepth = m_liveQueue.size();
    out.rebuildDepth = m_rebuildQueue.size();
    out.droppedLive = m_droppedLive;
    out.droppedRebuild = m_droppedRebuild;
    out.droppedQueueFull = m_droppedQueueFull;
    out.droppedMemorySoft = m_droppedMemorySoft;
    out.droppedMemoryHard = m_droppedMemoryHard;
    out.droppedWriterLag = m_droppedWriterLag;
    out.staleDropped = m_staleDropped;
    out.coalesced = m_coalesced;
    out.dispatchedLive = m_dispatchedLive;
    out.dispatchedRebuild = m_dispatchedRebuild;
    return out;
}

size_t PipelineSchedulerActor::totalDepth() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_liveQueue.size() + m_rebuildQueue.size();
}

} // namespace bs
