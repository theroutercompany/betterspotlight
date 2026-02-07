#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

#include "core/index/sqlite_store.h"
#include "core/indexing/indexer.h"
#include "core/indexing/chunker.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"
#include "core/shared/types.h"
#include "core/shared/chunk.h"

class TestIndexPersistence : public QObject {
    Q_OBJECT

private slots:
    void testIndexSurvivesReopen();
    void testItemMetadataSurvivesReopen();
    void testMultipleItemsSurviveReopen();
    void testHealthStatsSurviveReopen();
};

void TestIndexPersistence::testIndexSurvivesReopen()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = tempDir.path() + "/persist.db";
    const QString filePath = tempDir.path() + "/persist_doc.txt";

    // Create a real file so the indexer can stat() it
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Persistent content about cryptographic hash functions "
            << "including SHA-256 and BLAKE3 algorithms.";
    }

    // ── Phase 1: Open store, index file, close store ─────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        bs::ExtractionManager extractor;
        bs::PathRules pathRules;
        bs::Chunker chunker;
        bs::Indexer indexer(*store, extractor, pathRules, chunker);

        bs::WorkItem item;
        item.type = bs::WorkItem::Type::NewFile;
        item.filePath = filePath.toStdString();
        auto result = indexer.processWorkItem(item);
        QCOMPARE(result.status, bs::IndexResult::Status::Indexed);

        // Verify content is searchable before closing
        auto hitsBefore = store->searchFts5(QStringLiteral("cryptographic"));
        QVERIFY(!hitsBefore.empty());
    }
    // store goes out of scope, database is closed

    // ── Phase 2: Reopen store, verify data persists ──────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        // FTS5 search should still find the content
        auto hitsAfter = store->searchFts5(QStringLiteral("cryptographic"));
        QVERIFY(!hitsAfter.empty());

        // Additional search term from the indexed content
        auto hits2 = store->searchFts5(QStringLiteral("BLAKE3"));
        QVERIFY(!hits2.empty());

        // getItemByPath should still return the item
        auto item = store->getItemByPath(filePath);
        QVERIFY(item.has_value());
        QCOMPARE(item->name, QStringLiteral("persist_doc.txt"));
    }
}

void TestIndexPersistence::testItemMetadataSurvivesReopen()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = tempDir.path() + "/meta_persist.db";
    int64_t savedItemId = 0;

    // ── Phase 1: Insert item with known metadata ─────────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        auto id = store->upsertItem(
            QStringLiteral("/test/persist_meta.py"),
            QStringLiteral("persist_meta.py"),
            QStringLiteral("py"),
            bs::ItemKind::Code,
            4096,
            1700000000.0,
            1700001000.0);
        QVERIFY(id.has_value());
        savedItemId = *id;

        // Insert chunks
        std::vector<bs::Chunk> chunks;
        bs::Chunk c;
        c.chunkId = bs::computeChunkId(QStringLiteral("/test/persist_meta.py"), 0);
        c.filePath = QStringLiteral("/test/persist_meta.py");
        c.chunkIndex = 0;
        c.content = QStringLiteral("def fibonacci_recursive(n): return n if n < 2 else fibonacci_recursive(n-1) + fibonacci_recursive(n-2)");
        chunks.push_back(c);

        store->insertChunks(*id, QStringLiteral("persist_meta.py"),
                            QStringLiteral("/test/persist_meta.py"), chunks);
    }

    // ── Phase 2: Reopen and verify ───────────────────────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        auto item = store->getItemByPath(QStringLiteral("/test/persist_meta.py"));
        QVERIFY(item.has_value());
        QCOMPARE(item->id, savedItemId);
        QCOMPARE(item->name, QStringLiteral("persist_meta.py"));
        QCOMPARE(item->size, static_cast<int64_t>(4096));
        QCOMPARE(item->kind, QStringLiteral("code"));

        auto hits = store->searchFts5(QStringLiteral("fibonacci"));
        QVERIFY(!hits.empty());
    }
}

void TestIndexPersistence::testMultipleItemsSurviveReopen()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString dbPath = tempDir.path() + "/multi_persist.db";

    // ── Phase 1: Insert multiple items ───────────────────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        for (int i = 0; i < 5; ++i) {
            QString path = QStringLiteral("/test/file_%1.txt").arg(i);
            QString name = QStringLiteral("file_%1.txt").arg(i);

            auto id = store->upsertItem(
                path, name, QStringLiteral("txt"), bs::ItemKind::Text,
                100 * (i + 1), 1700000000.0, 1700000000.0 + i);
            QVERIFY(id.has_value());

            std::vector<bs::Chunk> chunks;
            bs::Chunk c;
            c.chunkId = bs::computeChunkId(path, 0);
            c.filePath = path;
            c.chunkIndex = 0;
            c.content = QStringLiteral("Content for file number %1 with unique keyword alpha%1beta").arg(i);
            chunks.push_back(c);
            store->insertChunks(*id, name, path, chunks);
        }
    }

    // ── Phase 2: Reopen and verify all items persist ─────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        auto health = store->getHealth();
        QCOMPARE(health.totalIndexedItems, static_cast<int64_t>(5));
        QCOMPARE(health.totalChunks, static_cast<int64_t>(5));

        // Search for each unique keyword
        for (int i = 0; i < 5; ++i) {
            QString query = QStringLiteral("alpha%1beta").arg(i);
            auto hits = store->searchFts5(query);
            QVERIFY2(!hits.empty(),
                     qPrintable(QStringLiteral("Expected to find '%1'").arg(query)));
        }
    }
}

void TestIndexPersistence::testHealthStatsSurviveReopen()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString dbPath = tempDir.path() + "/health_persist.db";

    // ── Phase 1: Create items and record a failure ───────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        auto id = store->upsertItem(
            QStringLiteral("/test/health.txt"),
            QStringLiteral("health.txt"),
            QStringLiteral("txt"),
            bs::ItemKind::Text, 50, 1.0, 2.0);
        QVERIFY(id.has_value());

        store->recordFailure(*id, QStringLiteral("extraction"),
                             QStringLiteral("test error"));

        auto health = store->getHealth();
        QCOMPARE(health.totalIndexedItems, static_cast<int64_t>(1));
        QCOMPARE(health.totalFailures, static_cast<int64_t>(1));
    }

    // ── Phase 2: Reopen and verify health stats ──────────────────
    {
        auto store = bs::SQLiteStore::open(dbPath);
        QVERIFY(store.has_value());

        auto health = store->getHealth();
        QCOMPARE(health.totalIndexedItems, static_cast<int64_t>(1));
        QCOMPARE(health.totalFailures, static_cast<int64_t>(1));
    }
}

QTEST_MAIN(TestIndexPersistence)
#include "test_index_persistence.moc"
