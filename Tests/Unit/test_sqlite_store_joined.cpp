#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <sqlite3.h>
#include "core/index/sqlite_store.h"
#include "core/index/schema.h"
#include "core/shared/chunk.h"

class TestSQLiteStoreJoined : public QObject {
    Q_OBJECT

private slots:
    void testJoinedSearchBasic();
    void testJoinedSearchTimeFilter();
    void testJoinedSearchTypeFilter();
    void testJoinedSearchPathFilter();
    void testJoinedSearchExcludePath();
    void testJoinedSearchNoFilters();
    void testBatchFrequencies();
    void testBatchFrequenciesEmpty();
};

static int64_t insertItemWithContent(bs::SQLiteStore& store,
                                     const QString& path,
                                     const QString& name,
                                     const QString& extension,
                                     bs::ItemKind kind,
                                     int64_t size,
                                     double modifiedAt,
                                     const QString& content,
                                     const QString& parentPath = {})
{
    auto id = store.upsertItem(path, name, extension, kind, size,
                               1700000000.0, modifiedAt,
                               /*contentHash=*/{},
                               /*sensitivity=*/QStringLiteral("normal"),
                               parentPath);
    Q_ASSERT(id.has_value());

    std::vector<bs::Chunk> chunks;
    bs::Chunk c;
    c.chunkId = bs::computeChunkId(path, 0);
    c.filePath = path;
    c.chunkIndex = 0;
    c.content = content;
    chunks.push_back(c);

    bool ok = store.insertChunks(*id, name, path, chunks);
    Q_ASSERT(ok);
    return *id;
}

void TestSQLiteStoreJoined::testJoinedSearchBasic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    int64_t id1 = insertItemWithContent(*store,
        QStringLiteral("/docs/alpha.txt"),
        QStringLiteral("alpha.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 1024, 1700001000.0,
        QStringLiteral("The quarterly budget analysis reveals strong performance"),
        QStringLiteral("/docs/"));

    int64_t id2 = insertItemWithContent(*store,
        QStringLiteral("/docs/beta.md"),
        QStringLiteral("beta.md"),
        QStringLiteral("md"),
        bs::ItemKind::Markdown, 2048, 1700002000.0,
        QStringLiteral("Annual budget review completed successfully"),
        QStringLiteral("/docs/"));

    insertItemWithContent(*store,
        QStringLiteral("/docs/gamma.pdf"),
        QStringLiteral("gamma.pdf"),
        QStringLiteral("pdf"),
        bs::ItemKind::Pdf, 4096, 1700003000.0,
        QStringLiteral("Unrelated content about weather patterns"),
        QStringLiteral("/docs/"));

    auto hits = store->searchFts5Joined(QStringLiteral("budget"), 20, false);
    QVERIFY(hits.size() >= 2);

    bool foundId1 = false;
    bool foundId2 = false;
    for (const auto& hit : hits) {
        QVERIFY(hit.fileId > 0);
        QVERIFY(!hit.path.isEmpty());
        QVERIFY(!hit.name.isEmpty());
        QVERIFY(!hit.kind.isEmpty());
        QVERIFY(hit.size > 0);
        QVERIFY(hit.modifiedAt > 0.0);

        if (hit.fileId == id1) {
            foundId1 = true;
            QCOMPARE(hit.path, QStringLiteral("/docs/alpha.txt"));
            QCOMPARE(hit.name, QStringLiteral("alpha.txt"));
            QCOMPARE(hit.kind, QStringLiteral("text"));
            QCOMPARE(hit.size, static_cast<int64_t>(1024));
            QCOMPARE(hit.parentPath, QStringLiteral("/docs/"));
        }
        if (hit.fileId == id2) {
            foundId2 = true;
            QCOMPARE(hit.path, QStringLiteral("/docs/beta.md"));
            QCOMPARE(hit.name, QStringLiteral("beta.md"));
            QCOMPARE(hit.kind, QStringLiteral("markdown"));
            QCOMPARE(hit.size, static_cast<int64_t>(2048));
        }
    }
    QVERIFY(foundId1);
    QVERIFY(foundId2);
}

void TestSQLiteStoreJoined::testJoinedSearchTimeFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    insertItemWithContent(*store,
        QStringLiteral("/files/old.txt"),
        QStringLiteral("old.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 100, 1600000000.0,
        QStringLiteral("This document discusses project milestones"));

    int64_t recentId = insertItemWithContent(*store,
        QStringLiteral("/files/recent.txt"),
        QStringLiteral("recent.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 200, 1700000000.0,
        QStringLiteral("Updated project milestones and deliverables"));

    bs::SearchOptions opts;
    opts.modifiedAfter = 1650000000.0;

    auto hits = store->searchFts5Joined(QStringLiteral("milestones"), 20, false, opts);
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits[0].fileId, recentId);

    bs::SearchOptions opts2;
    opts2.modifiedBefore = 1650000000.0;

    auto hits2 = store->searchFts5Joined(QStringLiteral("milestones"), 20, false, opts2);
    QCOMPARE(static_cast<int>(hits2.size()), 1);
    QVERIFY(hits2[0].fileId != recentId);
}

void TestSQLiteStoreJoined::testJoinedSearchTypeFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    insertItemWithContent(*store,
        QStringLiteral("/mixed/notes.txt"),
        QStringLiteral("notes.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 100, 1700001000.0,
        QStringLiteral("Architecture design patterns and principles"));

    int64_t mdId = insertItemWithContent(*store,
        QStringLiteral("/mixed/notes.md"),
        QStringLiteral("notes.md"),
        QStringLiteral("md"),
        bs::ItemKind::Markdown, 200, 1700002000.0,
        QStringLiteral("Architecture documentation with design patterns"));

    bs::SearchOptions opts;
    opts.fileTypes = {QStringLiteral("md")};

    auto hits = store->searchFts5Joined(QStringLiteral("architecture"), 20, false, opts);
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits[0].fileId, mdId);

    bs::SearchOptions opts2;
    opts2.fileTypes = {QStringLiteral(".txt")};

    auto hits2 = store->searchFts5Joined(QStringLiteral("architecture"), 20, false, opts2);
    QCOMPARE(static_cast<int>(hits2.size()), 1);
    QVERIFY(hits2[0].fileId != mdId);
}

void TestSQLiteStoreJoined::testJoinedSearchPathFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    int64_t projId = insertItemWithContent(*store,
        QStringLiteral("/projects/report.txt"),
        QStringLiteral("report.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 100, 1700001000.0,
        QStringLiteral("Comprehensive status report for stakeholders"),
        QStringLiteral("/projects/"));

    insertItemWithContent(*store,
        QStringLiteral("/archive/report.txt"),
        QStringLiteral("report.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 200, 1700002000.0,
        QStringLiteral("Archived status report from last quarter"),
        QStringLiteral("/archive/"));

    bs::SearchOptions opts;
    opts.includePaths = {QStringLiteral("/projects/")};

    auto hits = store->searchFts5Joined(QStringLiteral("report"), 20, false, opts);
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits[0].fileId, projId);
    QVERIFY(hits[0].path.startsWith(QStringLiteral("/projects/")));
}

void TestSQLiteStoreJoined::testJoinedSearchExcludePath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    int64_t keepId = insertItemWithContent(*store,
        QStringLiteral("/src/main.cpp"),
        QStringLiteral("main.cpp"),
        QStringLiteral("cpp"),
        bs::ItemKind::Code, 500, 1700001000.0,
        QStringLiteral("Implementation of the core algorithm module"),
        QStringLiteral("/src/"));

    insertItemWithContent(*store,
        QStringLiteral("/build/main.cpp"),
        QStringLiteral("main.cpp"),
        QStringLiteral("cpp"),
        bs::ItemKind::Code, 500, 1700001000.0,
        QStringLiteral("Generated build output with algorithm references"),
        QStringLiteral("/build/"));

    bs::SearchOptions opts;
    opts.excludePaths = {QStringLiteral("/build/")};

    auto hits = store->searchFts5Joined(QStringLiteral("algorithm"), 20, false, opts);
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits[0].fileId, keepId);
}

void TestSQLiteStoreJoined::testJoinedSearchNoFilters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    insertItemWithContent(*store,
        QStringLiteral("/a/one.txt"),
        QStringLiteral("one.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 100, 1700001000.0,
        QStringLiteral("Exploring machine learning fundamentals"));

    insertItemWithContent(*store,
        QStringLiteral("/b/two.md"),
        QStringLiteral("two.md"),
        QStringLiteral("md"),
        bs::ItemKind::Markdown, 200, 1700002000.0,
        QStringLiteral("Advanced machine learning techniques and applications"));

    auto hits = store->searchFts5Joined(QStringLiteral("machine"), 20, false);
    QCOMPARE(static_cast<int>(hits.size()), 2);

    for (const auto& hit : hits) {
        QVERIFY(!hit.path.isEmpty());
        QVERIFY(!hit.name.isEmpty());
        QVERIFY(!hit.kind.isEmpty());
        QVERIFY(hit.size > 0);
    }
}

void TestSQLiteStoreJoined::testBatchFrequencies()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    auto id1 = store->upsertItem(
        QStringLiteral("/test/freq1.txt"),
        QStringLiteral("freq1.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 100, 1.0, 2.0);
    QVERIFY(id1.has_value());

    auto id2 = store->upsertItem(
        QStringLiteral("/test/freq2.txt"),
        QStringLiteral("freq2.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 200, 1.0, 3.0);
    QVERIFY(id2.has_value());

    auto id3 = store->upsertItem(
        QStringLiteral("/test/freq3.txt"),
        QStringLiteral("freq3.txt"),
        QStringLiteral("txt"),
        bs::ItemKind::Text, 300, 1.0, 4.0);
    QVERIFY(id3.has_value());

    QVERIFY(store->incrementFrequency(*id1));
    QVERIFY(store->incrementFrequency(*id1));
    QVERIFY(store->incrementFrequency(*id1));
    QVERIFY(store->incrementFrequency(*id2));

    std::vector<int64_t> ids = {*id1, *id2, *id3};
    auto freqMap = store->getFrequenciesBatch(ids);

    QVERIFY(freqMap.count(*id1) == 1);
    QCOMPARE(freqMap[*id1].openCount, 3);
    QCOMPARE(freqMap[*id1].totalInteractions, 3);
    QVERIFY(freqMap[*id1].lastOpenedAt > 0.0);

    QVERIFY(freqMap.count(*id2) == 1);
    QCOMPARE(freqMap[*id2].openCount, 1);
    QCOMPARE(freqMap[*id2].totalInteractions, 1);

    QVERIFY(freqMap.count(*id3) == 0);
}

void TestSQLiteStoreJoined::testBatchFrequenciesEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + "/test.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    std::vector<int64_t> empty;
    auto freqMap = store->getFrequenciesBatch(empty);
    QVERIFY(freqMap.empty());
}

QTEST_MAIN(TestSQLiteStoreJoined)
#include "test_sqlite_store_joined.moc"
