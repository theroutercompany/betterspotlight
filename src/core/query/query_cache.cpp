#include "core/query/query_cache.h"

namespace bs {

QueryCache::QueryCache(QueryCacheConfig config)
    : m_config(config)
{
}

std::optional<QJsonObject> QueryCache::get(const QString& cacheKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_index.find(cacheKey);
    if (it == m_index.end()) {
        ++m_misses;
        return std::nullopt;
    }

    // Check TTL
    const auto now = std::chrono::steady_clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second->insertedAt);
    if (age.count() >= m_config.ttlSeconds) {
        // Expired — remove lazily
        m_list.erase(it->second);
        m_index.erase(it);
        ++m_misses;
        return std::nullopt;
    }

    // Move to front (most recently used)
    if (it->second != m_list.begin()) {
        m_list.splice(m_list.begin(), m_list, it->second);
    }

    ++m_hits;
    return it->second->value;
}

void QueryCache::put(const QString& cacheKey, const QJsonObject& response)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // If key already exists, update it
    auto existing = m_index.find(cacheKey);
    if (existing != m_index.end()) {
        m_list.erase(existing->second);
        m_index.erase(existing);
    }

    // Evict oldest entries if at capacity
    while (static_cast<int>(m_list.size()) >= m_config.maxEntries && !m_list.empty()) {
        const auto& back = m_list.back();
        m_index.erase(back.key);
        m_list.pop_back();
        ++m_evictions;
    }

    // Insert at front
    m_list.push_front({cacheKey, response, std::chrono::steady_clock::now()});
    m_index[cacheKey] = m_list.begin();
}

void QueryCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_list.clear();
    m_index.clear();
}

QueryCache::Stats QueryCache::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_hits, m_misses, m_evictions, static_cast<int>(m_list.size())};
}

} // namespace bs
