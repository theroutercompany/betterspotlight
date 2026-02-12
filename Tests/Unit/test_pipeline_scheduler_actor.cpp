#include <QtTest/QtTest>

#include "core/indexing/pipeline_scheduler_actor.h"

#include <atomic>
#include <thread>

namespace {

bs::WorkItem makeItem(const std::string& path)
{
    bs::WorkItem item;
    item.type = bs::WorkItem::Type::ModifiedContent;
    item.filePath = path;
    return item;
}

} // namespace

class TestPipelineSchedulerActor : public QObject {
    Q_OBJECT

private slots:
    void testLaneCapsAndDropReasons();
    void testLiveLaneDispatchBias();
    void testShutdownUnblocksBlockingDequeue();
};

void TestPipelineSchedulerActor::testLaneCapsAndDropReasons()
{
    bs::PipelineSchedulerConfig cfg;
    cfg.liveLaneCap = 1;
    cfg.rebuildLaneCap = 1;
    cfg.liveDispatchRatioPct = 70;

    bs::PipelineSchedulerActor actor(cfg);

    QVERIFY(actor.enqueue(makeItem("/tmp/a"), bs::PipelineLane::Live));
    QVERIFY(!actor.enqueue(makeItem("/tmp/b"), bs::PipelineLane::Live));

    QVERIFY(actor.enqueue(makeItem("/tmp/c"), bs::PipelineLane::Rebuild));
    QVERIFY(!actor.enqueue(makeItem("/tmp/d"), bs::PipelineLane::Rebuild));

    actor.recordDrop(bs::PipelineLane::Live, QStringLiteral("memory_soft"));
    actor.recordDrop(bs::PipelineLane::Rebuild, QStringLiteral("memory_hard"));
    actor.recordDrop(bs::PipelineLane::Rebuild, QStringLiteral("writer_lag"));
    actor.recordCoalesced();
    actor.recordStaleDropped();

    const bs::PipelineSchedulerStats stats = actor.stats();
    QCOMPARE(stats.droppedLive, static_cast<size_t>(2));
    QCOMPARE(stats.droppedRebuild, static_cast<size_t>(3));
    QCOMPARE(stats.droppedQueueFull, static_cast<size_t>(2));
    QCOMPARE(stats.droppedMemorySoft, static_cast<size_t>(1));
    QCOMPARE(stats.droppedMemoryHard, static_cast<size_t>(1));
    QCOMPARE(stats.droppedWriterLag, static_cast<size_t>(1));
    QCOMPARE(stats.coalesced, static_cast<size_t>(1));
    QCOMPARE(stats.staleDropped, static_cast<size_t>(1));
}

void TestPipelineSchedulerActor::testLiveLaneDispatchBias()
{
    bs::PipelineSchedulerConfig cfg;
    cfg.liveLaneCap = 200;
    cfg.rebuildLaneCap = 200;
    cfg.liveDispatchRatioPct = 70;

    bs::PipelineSchedulerActor actor(cfg);

    for (int i = 0; i < 100; ++i) {
        QVERIFY(actor.enqueue(makeItem("/tmp/live-" + std::to_string(i)), bs::PipelineLane::Live));
        QVERIFY(actor.enqueue(makeItem("/tmp/rebuild-" + std::to_string(i)), bs::PipelineLane::Rebuild));
    }

    int liveCount = 0;
    int rebuildCount = 0;
    for (int i = 0; i < 100; ++i) {
        auto item = actor.tryDequeue();
        QVERIFY(item.has_value());
        if (item->lane == bs::PipelineLane::Live) {
            ++liveCount;
        } else {
            ++rebuildCount;
        }
    }

    QVERIFY2(liveCount >= 65 && liveCount <= 75,
             "First dispatch window should preserve ~70/30 live/rebuild ratio");
    QCOMPARE(liveCount + rebuildCount, 100);
}

void TestPipelineSchedulerActor::testShutdownUnblocksBlockingDequeue()
{
    bs::PipelineSchedulerActor actor;
    std::atomic<bool> stopping{false};
    std::atomic<bool> paused{false};

    std::optional<bs::PipelineSchedulerActor::ScheduledItem> result;
    std::thread waiter([&]() {
        result = actor.dequeueBlocking(stopping, paused);
    });

    QTest::qWait(50);
    actor.shutdown();

    waiter.join();
    QVERIFY(!result.has_value());
}

QTEST_MAIN(TestPipelineSchedulerActor)
#include "test_pipeline_scheduler_actor.moc"
