#pragma once

#include "core/fs/file_monitor.h"
#include <CoreServices/CoreServices.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace bs {

// FileMonitorMacOS — FSEvents-based file monitor for macOS.
//
// Uses the CoreServices FSEvents API with file-level granularity.
// Events are coalesced with a configurable latency (default 0.5s)
// and delivered on a dedicated dispatch queue.
class FileMonitorMacOS final : public FileMonitor {
public:
    explicit FileMonitorMacOS(double latencySeconds = 0.5);
    ~FileMonitorMacOS() override;

    bool start(const std::vector<std::string>& roots,
               ChangeCallback callback) override;
    void stop() override;
    bool isRunning() const override;
    void setLastEventId(uint64_t eventId)
    {
        m_lastEventId = static_cast<FSEventStreamEventId>(eventId);
    }
    uint64_t lastEventId() const
    {
        return static_cast<uint64_t>(m_lastEventId);
    }

private:
    // FSEvents callback — static trampoline that forwards to the instance.
    static void fsEventsCallback(ConstFSEventStreamRef streamRef,
                                 void* clientCallBackInfo,
                                 size_t numEvents,
                                 void* eventPaths,
                                 const FSEventStreamEventFlags eventFlags[],
                                 const FSEventStreamEventId eventIds[]);

    // Process a batch of raw FSEvents into WorkItems and deliver them.
    void handleEvents(size_t numEvents,
                      char** paths,
                      const FSEventStreamEventFlags flags[],
                      const FSEventStreamEventId eventIds[]);

    // Determine the WorkItem::Type from FSEvents flags.
    static WorkItem::Type classifyEvent(FSEventStreamEventFlags flags);

    // Flush any buffered events to the callback.
    void flushPendingEvents();

    double m_latency;
    std::atomic<bool> m_running{false};

    // Protects m_stream, m_queue, m_callback
    mutable std::mutex m_mutex;
    FSEventStreamRef m_stream = nullptr;
    dispatch_queue_t m_queue = nullptr;
    ChangeCallback m_callback;
    std::vector<std::string> m_roots;
    FSEventStreamEventId m_lastEventId = kFSEventStreamEventIdSinceNow;

    // Debounce buffer: events accumulate here and are delivered
    // kDebounceMs after the last event arrives.
    static constexpr int kDebounceMs = 500;
    std::vector<WorkItem> m_pendingEvents;
    std::mutex m_bufferMutex;
    bool m_deliveryScheduled = false;
};

} // namespace bs
