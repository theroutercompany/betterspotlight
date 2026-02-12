#include <QtTest/QtTest>

#include "core/indexing/path_state_actor.h"

namespace {

bs::WorkItem makeItem(const std::string& path, bs::WorkItem::Type type)
{
    bs::WorkItem item;
    item.type = type;
    item.filePath = path;
    return item;
}

} // namespace

class TestPathStateActor : public QObject {
    Q_OBJECT

private slots:
    void testCoalescingAndFollowUpDispatch();
    void testStaleDetectionAndReset();
};

void TestPathStateActor::testCoalescingAndFollowUpDispatch()
{
    bs::PathStateActor actor;

    const std::string path = "/tmp/path-state-actor.txt";
    auto first = actor.onIngress(makeItem(path, bs::WorkItem::Type::NewFile));
    QVERIFY(first.has_value());
    QCOMPARE(first->generation, static_cast<uint64_t>(1));

    auto coalesced = actor.onIngress(makeItem(path, bs::WorkItem::Type::ModifiedContent));
    QVERIFY(!coalesced.has_value());
    QCOMPARE(actor.pendingMergedCount(), static_cast<size_t>(1));

    bs::PreparedWork prepared;
    prepared.path = QString::fromStdString(path);
    prepared.generation = first->generation;

    auto followUp = actor.onPrepCompleted(prepared);
    QVERIFY(followUp.has_value());
    QCOMPARE(followUp->item.type, bs::WorkItem::Type::ModifiedContent);
    QCOMPARE(followUp->generation, static_cast<uint64_t>(2));
    QCOMPARE(actor.pendingMergedCount(), static_cast<size_t>(0));
}

void TestPathStateActor::testStaleDetectionAndReset()
{
    bs::PathStateActor actor;

    const std::string path = "/tmp/path-state-stale.txt";
    auto first = actor.onIngress(makeItem(path, bs::WorkItem::Type::ModifiedContent));
    QVERIFY(first.has_value());

    auto second = actor.onIngress(makeItem(path, bs::WorkItem::Type::ModifiedContent));
    QVERIFY(!second.has_value());

    bs::PreparedWork stale;
    stale.path = QString::fromStdString(path);
    stale.generation = 1;

    QVERIFY(actor.isStalePrepared(stale));

    actor.reset();
    QVERIFY(!actor.isStalePrepared(stale));
}

QTEST_MAIN(TestPathStateActor)
#include "test_path_state_actor.moc"
