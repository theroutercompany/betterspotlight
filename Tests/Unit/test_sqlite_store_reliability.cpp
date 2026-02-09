#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QThread>
#include <sqlite3.h>
#include "core/index/sqlite_store.h"
#include "core/index/schema.h"
#include "core/shared/chunk.h"

class TestSQLiteStoreReliability : public QObject {
    Q_OBJECT

private slots:
    void testStepWithRetryCompiles();
    void testFts5IntegrityCheckPasses();
    void testFts5IntegrityCheckOnEmptyStore();
    void testWalCheckpoint();
    void testConcurrentWriterBusy();
};

void TestSQLiteStoreReliability::testStepWithRetryCompiles()
{
    // Verify that stepWithRetry works for a simple insert via upsertItem
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    auto id = store->upsertItem(
        QStringLiteral("/tmp/retry_test.txt"),
        QStringLiteral("retry_test.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text,
        100,
        1000.0,
        2000.0);
    QVERIFY(id.has_value());
    QVERIFY(*id > 0);
}

void TestSQLiteStoreReliability::testFts5IntegrityCheckPasses()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    // Insert an item and some chunks
    auto id = store->upsertItem(
        QStringLiteral("/tmp/fts5_check.txt"),
        QStringLiteral("fts5_check.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text,
        512,
        1000.0,
        2000.0);
    QVERIFY(id.has_value());

    bs::Chunk chunk;
    chunk.chunkIndex = 0;
    chunk.content = QStringLiteral("Hello world test content for FTS5 integrity check");
    chunk.chunkId = QStringLiteral("chunk-fts5-0");

    QVERIFY(store->insertChunks(*id, QStringLiteral("fts5_check.txt"),
                                 QStringLiteral("/tmp/fts5_check.txt"),
                                 {chunk}));

    // FTS5 integrity check should pass
    QVERIFY(store->fts5IntegrityCheck());
}

void TestSQLiteStoreReliability::testFts5IntegrityCheckOnEmptyStore()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    // Empty FTS5 index should also pass integrity check
    QVERIFY(store->fts5IntegrityCheck());
}

void TestSQLiteStoreReliability::testWalCheckpoint()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    // Insert some data to create WAL entries
    auto id = store->upsertItem(
        QStringLiteral("/tmp/wal_test.txt"),
        QStringLiteral("wal_test.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text,
        256,
        1000.0,
        2000.0);
    QVERIFY(id.has_value());

    // WAL checkpoint should succeed
    QVERIFY(store->walCheckpoint());

    // Database should still be functional after checkpoint
    auto item = store->getItemByPath(QStringLiteral("/tmp/wal_test.txt"));
    QVERIFY(item.has_value());
    QCOMPARE(item->name, QStringLiteral("wal_test.txt"));
}

void TestSQLiteStoreReliability::testConcurrentWriterBusy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";

    // Open first connection (the store)
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    // Open a second raw connection that will hold a write lock
    sqlite3* db2 = nullptr;
    int rc = sqlite3_open(dbPath.toUtf8().constData(), &db2);
    QCOMPARE(rc, SQLITE_OK);

    // Set a short busy timeout on connection 2
    sqlite3_busy_timeout(db2, 100);

    // Set WAL mode and start an immediate transaction to hold the write lock
    char* errMsg = nullptr;
    rc = sqlite3_exec(db2, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
    QCOMPARE(rc, SQLITE_OK);
    sqlite3_free(errMsg);

    rc = sqlite3_exec(db2, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg);
    QCOMPARE(rc, SQLITE_OK);
    sqlite3_free(errMsg);

    // Insert via the second connection to hold the write lock
    rc = sqlite3_exec(db2,
        "INSERT OR REPLACE INTO settings (key, value) VALUES ('test_lock', 'locked');",
        nullptr, nullptr, &errMsg);
    QCOMPARE(rc, SQLITE_OK);
    sqlite3_free(errMsg);

    // Release the lock after a short delay in a separate thread
    QThread* releaser = QThread::create([db2]() {
        QThread::msleep(100);
        sqlite3_exec(db2, "COMMIT;", nullptr, nullptr, nullptr);
    });
    releaser->start();

    // This upsert should succeed because stepWithRetry retries on BUSY
    auto id = store->upsertItem(
        QStringLiteral("/tmp/concurrent_test.txt"),
        QStringLiteral("concurrent_test.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text,
        100,
        1000.0,
        2000.0);

    releaser->wait();
    delete releaser;

    QVERIFY(id.has_value());

    // Verify the item was actually written
    auto item = store->getItemByPath(QStringLiteral("/tmp/concurrent_test.txt"));
    QVERIFY(item.has_value());

    sqlite3_close(db2);
}

QTEST_MAIN(TestSQLiteStoreReliability)
#include "test_sqlite_store_reliability.moc"
