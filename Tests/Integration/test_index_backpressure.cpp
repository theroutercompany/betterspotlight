#include <QtTest/QtTest>

#include "core/indexing/work_queue.h"

#include <string>

class TestIndexBackpressure : public QObject {
    Q_OBJECT

private slots:
    void testPrimaryEnqueueEvictsRescanUnderPressure();
    void testPrimaryEnqueueFailsWhenQueueContainsOnlyPrimaryItems();
};

void TestIndexBackpressure::testPrimaryEnqueueEvictsRescanUnderPressure()
{
    bs::WorkQueue queue;

    for (size_t i = 0; i < bs::WorkQueue::MAX_QUEUE_SIZE; ++i) {
        bs::WorkItem item;
        item.type = bs::WorkItem::Type::RescanDirectory;
        item.filePath = "/tmp/rescan-" + std::to_string(i);
        QVERIFY(queue.enqueue(std::move(item)));
    }

    bs::WorkItem primary;
    primary.type = bs::WorkItem::Type::NewFile;
    primary.filePath = "/tmp/primary.txt";
    QVERIFY(queue.enqueue(primary));

    const bs::QueueStats stats = queue.stats();
    QCOMPARE(stats.depth, bs::WorkQueue::MAX_QUEUE_SIZE);
    QVERIFY(stats.droppedItems >= 1);

    auto dequeued = queue.dequeue();
    QVERIFY(dequeued.has_value());
    QCOMPARE(dequeued->type, bs::WorkItem::Type::NewFile);
    queue.markItemComplete();
}

void TestIndexBackpressure::testPrimaryEnqueueFailsWhenQueueContainsOnlyPrimaryItems()
{
    bs::WorkQueue queue;

    for (size_t i = 0; i < bs::WorkQueue::MAX_QUEUE_SIZE; ++i) {
        bs::WorkItem item;
        item.type = bs::WorkItem::Type::NewFile;
        item.filePath = "/tmp/file-" + std::to_string(i);
        QVERIFY(queue.enqueue(std::move(item)));
    }

    bs::WorkItem overflow;
    overflow.type = bs::WorkItem::Type::ModifiedContent;
    overflow.filePath = "/tmp/overflow.txt";
    QVERIFY(!queue.enqueue(overflow));

    const bs::QueueStats stats = queue.stats();
    QVERIFY(stats.droppedItems >= 1);
}

QTEST_MAIN(TestIndexBackpressure)
#include "test_index_backpressure.moc"
