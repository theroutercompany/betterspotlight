#pragma once

#include "core/shared/types.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace bs {

struct QueueStats {
    size_t depth = 0;
    size_t activeItems = 0;
    size_t droppedItems = 0;
    bool isPaused = false;
};

// WorkQueue — thread-safe priority queue for scheduling indexing work items.
//
// Priority ordering (highest to lowest):
//   Delete (0) > ModifiedContent (1) > NewFile (2) > RescanDirectory (3)
//
// Backpressure: when at MAX_QUEUE_SIZE, the lowest-priority items
// (RescanDirectory first) are dropped to make room. If the queue is
// still full after eviction, the new item is refused.
//
// Pause/resume: when paused, dequeue() blocks and returns nullopt only
// when shutdown() is called. On resume, blocked dequeue() threads wake up.
class WorkQueue {
public:
    static constexpr size_t MAX_QUEUE_SIZE = 10000;

    WorkQueue() = default;
    ~WorkQueue();

    // Non-copyable, non-movable
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;
    WorkQueue(WorkQueue&&) = delete;
    WorkQueue& operator=(WorkQueue&&) = delete;

    // Add an item to the queue. If at capacity, drops lowest-priority items
    // to make room. Returns true if the item was enqueued, false if dropped.
    bool enqueue(WorkItem item);

    // Blocking dequeue. Returns nullopt when paused (and not resumed) or
    // when shutdown() has been called.
    std::optional<WorkItem> dequeue();

    // Marks one dequeued item as fully processed.
    void markItemComplete();

    // Pause processing: dequeue() will block until resume() or shutdown().
    void pause();

    // Resume processing: unblocks any waiting dequeue() calls.
    void resume();

    // Returns true if the queue is currently paused.
    bool isPaused() const;

    // Unblock all waiting threads and signal permanent shutdown.
    // After shutdown(), dequeue() always returns nullopt.
    void shutdown();

    // Number of items currently in the queue.
    size_t size() const;

    // Alias for size().
    size_t pendingCount() const;

    // Snapshot of queue statistics.
    QueueStats stats() const;

private:
    // Comparator: lower numeric Type value = higher priority.
    // std::priority_queue is a max-heap, so we invert the comparison
    // so that Delete (0) is dequeued before RescanDirectory (3).
    struct LowerPriorityFirst {
        bool operator()(const WorkItem& a, const WorkItem& b) const {
            return static_cast<int>(a.type) > static_cast<int>(b.type);
        }
    };

    // Drop one lowest-priority item from the queue. Returns true if
    // an item was dropped.
    bool dropLowestPriority();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

    std::priority_queue<WorkItem, std::vector<WorkItem>, LowerPriorityFirst> m_queue;
    size_t m_droppedItems = 0;
    size_t m_activeItems = 0;
    bool m_paused = false;
    bool m_shutdown = false;
};

} // namespace bs
