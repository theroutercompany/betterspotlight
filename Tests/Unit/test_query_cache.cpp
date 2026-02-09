#include <QtTest/QtTest>
#include "core/query/query_cache.h"

#include <QJsonArray>
#include <thread>

class TestQueryCache : public QObject {
    Q_OBJECT

private:
    QJsonObject makeResult(const QString& query, int count) const
    {
        QJsonObject obj;
        obj[QStringLiteral("query")] = query;
        obj[QStringLiteral("totalMatches")] = count;
        obj[QStringLiteral("results")] = QJsonArray();
        return obj;
    }

private slots:
    void testCacheHitReturnsSameResult()
    {
        bs::QueryCache cache;
        QJsonObject original = makeResult(QStringLiteral("test"), 5);
        cache.put(QStringLiteral("test|0"), original);

        auto result = cache.get(QStringLiteral("test|0"));
        QVERIFY(result.has_value());
        QCOMPARE(result->value(QStringLiteral("query")).toString(),
                 QStringLiteral("test"));
        QCOMPARE(result->value(QStringLiteral("totalMatches")).toInt(), 5);
    }

    void testCacheMissReturnsNullopt()
    {
        bs::QueryCache cache;
        auto result = cache.get(QStringLiteral("nonexistent"));
        QVERIFY(!result.has_value());
    }

    void testCacheTTLExpiration()
    {
        bs::QueryCacheConfig config;
        config.ttlSeconds = 1;  // 1 second TTL
        bs::QueryCache cache(config);

        cache.put(QStringLiteral("key"), makeResult(QStringLiteral("val"), 1));

        // Should be present immediately
        QVERIFY(cache.get(QStringLiteral("key")).has_value());

        // Wait for TTL to expire
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        // Should be gone
        QVERIFY(!cache.get(QStringLiteral("key")).has_value());
    }

    void testCacheLRUEviction()
    {
        bs::QueryCacheConfig config;
        config.maxEntries = 3;
        config.ttlSeconds = 60;
        bs::QueryCache cache(config);

        cache.put(QStringLiteral("a"), makeResult(QStringLiteral("a"), 1));
        cache.put(QStringLiteral("b"), makeResult(QStringLiteral("b"), 2));
        cache.put(QStringLiteral("c"), makeResult(QStringLiteral("c"), 3));

        // All three should be present
        QVERIFY(cache.get(QStringLiteral("a")).has_value());
        QVERIFY(cache.get(QStringLiteral("b")).has_value());
        QVERIFY(cache.get(QStringLiteral("c")).has_value());

        // Adding a 4th should evict the least recently used
        // After the gets above, "a" was accessed first, then "b", then "c"
        // So "a" was accessed most recently... Actually LRU order after gets:
        // front(MRU) -> c -> b -> a <- LRU... no, after get(a), get(b), get(c):
        // front -> c -> b -> a
        // So inserting "d" evicts "a" (LRU)
        cache.put(QStringLiteral("d"), makeResult(QStringLiteral("d"), 4));

        QVERIFY(!cache.get(QStringLiteral("a")).has_value()); // evicted
        QVERIFY(cache.get(QStringLiteral("b")).has_value());
        QVERIFY(cache.get(QStringLiteral("c")).has_value());
        QVERIFY(cache.get(QStringLiteral("d")).has_value());
    }

    void testCacheClearRemovesAll()
    {
        bs::QueryCache cache;
        cache.put(QStringLiteral("a"), makeResult(QStringLiteral("a"), 1));
        cache.put(QStringLiteral("b"), makeResult(QStringLiteral("b"), 2));

        cache.clear();

        QVERIFY(!cache.get(QStringLiteral("a")).has_value());
        QVERIFY(!cache.get(QStringLiteral("b")).has_value());

        auto stats = cache.stats();
        QCOMPARE(stats.currentSize, 0);
    }

    void testCacheKeyDiffersByMode()
    {
        bs::QueryCache cache;
        cache.put(QStringLiteral("test|0"), makeResult(QStringLiteral("mode0"), 1));
        cache.put(QStringLiteral("test|1"), makeResult(QStringLiteral("mode1"), 2));

        auto r0 = cache.get(QStringLiteral("test|0"));
        auto r1 = cache.get(QStringLiteral("test|1"));

        QVERIFY(r0.has_value());
        QVERIFY(r1.has_value());
        QCOMPARE(r0->value(QStringLiteral("query")).toString(), QStringLiteral("mode0"));
        QCOMPARE(r1->value(QStringLiteral("query")).toString(), QStringLiteral("mode1"));
    }

    void testCacheStats()
    {
        bs::QueryCache cache;

        // Initial stats
        auto s = cache.stats();
        QCOMPARE(s.hits, uint64_t(0));
        QCOMPARE(s.misses, uint64_t(0));
        QCOMPARE(s.currentSize, 0);

        // Miss
        cache.get(QStringLiteral("missing"));
        s = cache.stats();
        QCOMPARE(s.misses, uint64_t(1));

        // Put + hit
        cache.put(QStringLiteral("key"), makeResult(QStringLiteral("val"), 1));
        cache.get(QStringLiteral("key"));
        s = cache.stats();
        QCOMPARE(s.hits, uint64_t(1));
        QCOMPARE(s.currentSize, 1);
    }

    void testCacheEvictionStats()
    {
        bs::QueryCacheConfig config;
        config.maxEntries = 2;
        config.ttlSeconds = 60;
        bs::QueryCache cache(config);

        cache.put(QStringLiteral("a"), makeResult(QStringLiteral("a"), 1));
        cache.put(QStringLiteral("b"), makeResult(QStringLiteral("b"), 2));
        cache.put(QStringLiteral("c"), makeResult(QStringLiteral("c"), 3)); // evicts "a"

        auto s = cache.stats();
        QCOMPARE(s.evictions, uint64_t(1));
        QCOMPARE(s.currentSize, 2);
    }
};

QTEST_MAIN(TestQueryCache)
#include "test_query_cache.moc"
