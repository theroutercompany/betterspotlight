// BetterSpotlight test fixture: C++ source file.
//
// Implements a thread-safe bounded buffer (producer-consumer pattern)
// with condition variable synchronization.

#include <condition_variable>
#include <mutex>
#include <queue>
#include <optional>
#include <chrono>
#include <string>

namespace fixture {

/// Thread-safe bounded buffer for producer-consumer patterns.
/// Uses condition variables for efficient wait/notify semantics.
template <typename T>
class BoundedBuffer {
public:
    explicit BoundedBuffer(size_t capacity)
        : m_capacity(capacity)
    {
    }

    /// Push an item into the buffer. Blocks if the buffer is full.
    void push(T item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < m_capacity; });
        m_queue.push(std::move(item));
        m_notEmpty.notify_one();
    }

    /// Pop an item from the buffer. Blocks if the buffer is empty.
    T pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return !m_queue.empty(); });
        T item = std::move(m_queue.front());
        m_queue.pop();
        m_notFull.notify_one();
        return item;
    }

    /// Try to pop with a timeout. Returns nullopt if timed out.
    std::optional<T> tryPop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_notEmpty.wait_for(lock, timeout, [this] { return !m_queue.empty(); })) {
            return std::nullopt;
        }
        T item = std::move(m_queue.front());
        m_queue.pop();
        m_notFull.notify_one();
        return item;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    size_t capacity() const { return m_capacity; }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::queue<T> m_queue;
    size_t m_capacity;
};

/// Demonstrates usage of BoundedBuffer with string messages.
struct MessageProcessor {
    BoundedBuffer<std::string> inbox{128};
    BoundedBuffer<std::string> outbox{64};

    void processMessage(const std::string& msg) {
        // Simulate processing: reverse the message
        std::string reversed(msg.rbegin(), msg.rend());
        outbox.push(reversed);
    }
};

} // namespace fixture
