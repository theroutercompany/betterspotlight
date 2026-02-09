#pragma once

#include <QJsonObject>
#include <QString>

#include <chrono>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace bs {

struct QueryCacheConfig {
    int maxEntries = 128;
    int ttlSeconds = 30;
};

class QueryCache {
public:
    explicit QueryCache(QueryCacheConfig config = {});

    // Returns cached result or nullopt. Lazily evicts expired entries.
    std::optional<QJsonObject> get(const QString& cacheKey);
    void put(const QString& cacheKey, const QJsonObject& response);
    void clear();

    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        int currentSize = 0;
    };
    Stats stats() const;

private:
    struct Entry {
        QString key;
        QJsonObject value;
        std::chrono::steady_clock::time_point insertedAt;
    };

    QueryCacheConfig m_config;
    mutable std::mutex m_mutex;
    std::list<Entry> m_list;  // front = most recently used

    struct QStringHash {
        size_t operator()(const QString& s) const { return qHash(s); }
    };
    std::unordered_map<QString, std::list<Entry>::iterator, QStringHash> m_index;

    uint64_t m_hits = 0;
    uint64_t m_misses = 0;
    uint64_t m_evictions = 0;
};

} // namespace bs
