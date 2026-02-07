#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <QThread>

#include "core/index/sqlite_store.h"
#include "core/indexing/indexer.h"
#include "core/indexing/chunker.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"
#include "core/shared/types.h"
#include "core/shared/chunk.h"

class TestIncrementalUpdate : public QObject {
    Q_OBJECT

private slots:
    void testModifiedFileReindexed();
    void testDeletedFileRemovedFromIndex();
    void testModifiedFileUpdatesMetadata();
    void testMultipleModificationsToSameFile();
};

void TestIncrementalUpdate::testModifiedFileReindexed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/evolving_document.txt";
    const QString dbPath = tempDir.path() + "/incr.db";

    // ── Phase 1: Create file with initial content and index ──────
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Initial content about machine learning algorithms "
            << "including gradient descent and backpropagation.";
    }

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();
    auto result1 = indexer.processWorkItem(item);
    QCOMPARE(result1.status, bs::IndexResult::Status::Indexed);

    // Verify initial content is searchable
    auto hits1 = store->searchFts5(QStringLiteral("backpropagation"));
    QVERIFY(!hits1.empty());

    // ── Phase 2: Modify the file with different content ──────────
    // Need to wait briefly so mtime changes
    QThread::msleep(1100);

    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Completely new content about distributed systems "
            << "including consensus protocols and byzantine fault tolerance.";
    }

    // Process as ModifiedContent
    bs::WorkItem modItem;
    modItem.type = bs::WorkItem::Type::ModifiedContent;
    modItem.filePath = filePath.toStdString();
    auto result2 = indexer.processWorkItem(modItem);
    QCOMPARE(result2.status, bs::IndexResult::Status::Indexed);

    // ── Verify: old content is gone, new content is found ────────
    auto oldHits = store->searchFts5(QStringLiteral("backpropagation"));
    QVERIFY(oldHits.empty());

    auto newHits = store->searchFts5(QStringLiteral("byzantine"));
    QVERIFY(!newHits.empty());
}

void TestIncrementalUpdate::testDeletedFileRemovedFromIndex()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/deletable_file.txt";
    const QString dbPath = tempDir.path() + "/del.db";

    // Create and index a file
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Ephemeral content with unique keyword xyzzy_delete_test.";
    }

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();
    indexer.processWorkItem(item);

    // Verify it was indexed
    auto hitsBefore = store->searchFts5(QStringLiteral("xyzzy_delete_test"));
    QVERIFY(!hitsBefore.empty());

    // Process a Delete work item
    bs::WorkItem delItem;
    delItem.type = bs::WorkItem::Type::Delete;
    delItem.filePath = filePath.toStdString();
    auto result = indexer.processWorkItem(delItem);
    QCOMPARE(result.status, bs::IndexResult::Status::Deleted);

    // Verify: search no longer finds the content
    auto hitsAfter = store->searchFts5(QStringLiteral("xyzzy_delete_test"));
    QVERIFY(hitsAfter.empty());

    // Verify: item no longer in the database
    auto row = store->getItemByPath(filePath);
    QVERIFY(!row.has_value());
}

void TestIncrementalUpdate::testModifiedFileUpdatesMetadata()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/metadata_update.txt";
    const QString dbPath = tempDir.path() + "/meta_upd.db";

    // Phase 1: Create small file
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Small initial content.";
    }

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();
    indexer.processWorkItem(item);

    auto row1 = store->getItemByPath(filePath);
    QVERIFY(row1.has_value());
    int64_t originalSize = row1->size;
    double originalModifiedAt = row1->modifiedAt;

    // Wait for mtime to change
    QThread::msleep(1100);

    // Phase 2: Write much larger content
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        for (int i = 0; i < 100; ++i) {
            out << "Line " << i << ": This is significantly more content than before.\n";
        }
    }

    bs::WorkItem modItem;
    modItem.type = bs::WorkItem::Type::ModifiedContent;
    modItem.filePath = filePath.toStdString();
    indexer.processWorkItem(modItem);

    auto row2 = store->getItemByPath(filePath);
    QVERIFY(row2.has_value());

    // Size should have increased
    QVERIFY(row2->size > originalSize);
    // modifiedAt should have changed
    QVERIFY(row2->modifiedAt > originalModifiedAt);
}

void TestIncrementalUpdate::testMultipleModificationsToSameFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/multi_mod.txt";
    const QString dbPath = tempDir.path() + "/multi_mod.db";

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    // Version 1
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Version one: alpha omega gamma.";
    }

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();
    indexer.processWorkItem(item);

    auto hits_v1 = store->searchFts5(QStringLiteral("alpha"));
    QVERIFY(!hits_v1.empty());

    QThread::msleep(1100);

    // Version 2
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Version two: delta epsilon zeta.";
    }

    bs::WorkItem modItem1;
    modItem1.type = bs::WorkItem::Type::ModifiedContent;
    modItem1.filePath = filePath.toStdString();
    indexer.processWorkItem(modItem1);

    QVERIFY(store->searchFts5(QStringLiteral("alpha")).empty());
    QVERIFY(!store->searchFts5(QStringLiteral("delta")).empty());

    QThread::msleep(1100);

    // Version 3
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Version three: theta iota kappa.";
    }

    bs::WorkItem modItem2;
    modItem2.type = bs::WorkItem::Type::ModifiedContent;
    modItem2.filePath = filePath.toStdString();
    indexer.processWorkItem(modItem2);

    QVERIFY(store->searchFts5(QStringLiteral("delta")).empty());
    QVERIFY(!store->searchFts5(QStringLiteral("theta")).empty());

    // Still only one item in the database
    auto health = store->getHealth();
    QCOMPARE(health.totalIndexedItems, static_cast<int64_t>(1));
}

QTEST_MAIN(TestIncrementalUpdate)
#include "test_incremental_update.moc"
