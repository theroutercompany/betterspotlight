#include "core/indexing/work_queue.h"
#include "core/shared/logging.h"

#include <algorithm>

namespace bs {

// ── Destruction ─────────────────────────────────────────────

WorkQueue::~WorkQueue()
{
    shutdown();
}

// ── Enqueue ─────────────────────────────────────────────────

bool WorkQueue::enqueue(WorkItem item)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_shutdown) {
        LOG_WARN(bsIndex, "WorkQueue::enqueue() called after shutdown");
        return false;
    }

    // Backpressure: if at capacity, try to drop lowest-priority items
    if (m_queue.size() >= MAX_QUEUE_SIZE) {
        if (!dropLowestPriority()) {
            // Could not free space — refuse the new item
            ++m_droppedItems;
            LOG_WARN(bsIndex, "WorkQueue at capacity (%d), dropped item: %s",
                     static_cast<int>(MAX_QUEUE_SIZE), item.filePath.c_str());
            return false;
        }
    }

    // Per-item enqueue logging is too noisy for home-dir scans (100K+ files).
    // Queue depth is reported at batch-commit boundaries in the processing loop.

    m_queue.push(std::move(item));
    m_cv.notify_one();
    return true;
}

// ── Dequeue ─────────────────────────────────────────────────

std::optional<WorkItem> WorkQueue::dequeue()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_cv.wait(lock, [this] {
        return m_shutdown || (!m_paused && !m_queue.empty());
    });

    if (m_shutdown || m_paused || m_queue.empty()) {
        return std::nullopt;
    }

    WorkItem item = m_queue.top();
    m_queue.pop();
    ++m_activeItems;

    LOG_DEBUG(bsIndex, "Dequeue %s (type=%d, queue depth=%d)",
              item.filePath.c_str(),
              static_cast<int>(item.type),
              static_cast<int>(m_queue.size()));

    return item;
}

void WorkQueue::markItemComplete()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeItems > 0) {
        --m_activeItems;
    }
}

// ── Pause / resume ──────────────────────────────────────────

void WorkQueue::pause()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_paused) {
        m_paused = true;
        LOG_INFO(bsIndex, "WorkQueue paused (depth=%d)", static_cast<int>(m_queue.size()));
    }
}

void WorkQueue::resume()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_paused) {
        m_paused = false;
        LOG_INFO(bsIndex, "WorkQueue resumed (depth=%d)", static_cast<int>(m_queue.size()));
        m_cv.notify_all();
    }
}

bool WorkQueue::isPaused() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_paused;
}

// ── Shutdown ────────────────────────────────────────────────

void WorkQueue::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_shutdown) {
        m_shutdown = true;
        LOG_INFO(bsIndex, "WorkQueue shutting down (depth=%d, dropped=%d)",
                 static_cast<int>(m_queue.size()),
                 static_cast<int>(m_droppedItems));
        m_cv.notify_all();
    }
}

// ── Size / stats ────────────────────────────────────────────

size_t WorkQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

size_t WorkQueue::pendingCount() const
{
    return size();
}

QueueStats WorkQueue::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QueueStats s;
    s.depth = m_queue.size();
    s.activeItems = m_activeItems;
    s.droppedItems = m_droppedItems;
    s.isPaused = m_paused;
    return s;
}

// ── Private helpers ─────────────────────────────────────────

bool WorkQueue::dropLowestPriority()
{
    // The priority queue gives us the highest-priority item at the top.
    // To drop the lowest-priority item we need to rebuild the queue
    // without the lowest-priority entry.
    //
    // Strategy: drain into a vector, remove the last (lowest-priority)
    // element, re-insert. This is O(n) but only fires under backpressure
    // which is an exceptional condition.

    if (m_queue.empty()) {
        return false;
    }

    // Find the lowest-priority item by draining
    std::vector<WorkItem> items;
    items.reserve(m_queue.size());
    while (!m_queue.empty()) {
        items.push_back(m_queue.top());
        m_queue.pop();
    }

    // Items are in priority order (highest first). The last element
    // is the lowest priority. Only drop RescanDirectory items to avoid
    // losing deletes or content updates.
    size_t dropIdx = items.size();
    for (size_t i = items.size(); i > 0; --i) {
        if (items[i - 1].type == WorkItem::Type::RescanDirectory) {
            dropIdx = i - 1;
            break;
        }
    }

    bool dropped = false;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i == dropIdx) {
            ++m_droppedItems;
            dropped = true;
            LOG_DEBUG(bsIndex, "Backpressure: dropped RescanDirectory item: %s",
                      items[i].filePath.c_str());
            continue;
        }
        m_queue.push(std::move(items[i]));
    }

    return dropped;
}

} // namespace bs
