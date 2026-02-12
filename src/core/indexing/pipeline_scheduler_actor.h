#pragma once

#include "core/shared/types.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace bs {

enum class PipelineLane {
    Live,
    Rebuild,
};

struct PipelineSchedulerConfig {
    size_t liveLaneCap = 4000;
    size_t rebuildLaneCap = 20000;
    int liveDispatchRatioPct = 70;
};

struct PipelineSchedulerStats {
    size_t liveDepth = 0;
    size_t rebuildDepth = 0;
    size_t droppedLive = 0;
    size_t droppedRebuild = 0;
    size_t droppedQueueFull = 0;
    size_t droppedMemorySoft = 0;
    size_t droppedMemoryHard = 0;
    size_t droppedWriterLag = 0;
    size_t staleDropped = 0;
    size_t coalesced = 0;
    size_t dispatchedLive = 0;
    size_t dispatchedRebuild = 0;
};

class PipelineSchedulerActor : public QObject {
    Q_OBJECT
public:
    struct ScheduledItem {
        WorkItem item;
        PipelineLane lane = PipelineLane::Live;
    };

    explicit PipelineSchedulerActor(const PipelineSchedulerConfig& config = {},
                                    QObject* parent = nullptr);
    ~PipelineSchedulerActor() override;

    bool enqueue(const WorkItem& item, PipelineLane lane);
    std::optional<ScheduledItem> dequeueBlocking(std::atomic<bool>& stopping,
                                                 std::atomic<bool>& paused);
    std::optional<ScheduledItem> tryDequeue();
    void shutdown();
    void notifyAll();

    void recordDrop(PipelineLane lane, const QString& reason);
    void recordCoalesced();
    void recordStaleDropped();
    PipelineSchedulerStats stats() const;
    size_t totalDepth() const;

private:
    std::optional<ScheduledItem> popNextLocked();

    PipelineSchedulerConfig m_config;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<WorkItem> m_liveQueue;
    std::deque<WorkItem> m_rebuildQueue;
    bool m_shutdown = false;

    size_t m_droppedLive = 0;
    size_t m_droppedRebuild = 0;
    size_t m_droppedQueueFull = 0;
    size_t m_droppedMemorySoft = 0;
    size_t m_droppedMemoryHard = 0;
    size_t m_droppedWriterLag = 0;
    size_t m_staleDropped = 0;
    size_t m_coalesced = 0;
    size_t m_dispatchedLive = 0;
    size_t m_dispatchedRebuild = 0;
    int m_dispatchCycle = 0;
};

} // namespace bs
