#include <QtTest/QtTest>

#include "core/index/sqlite_store.h"
#include "core/shared/chunk.h"
#include "core/shared/search_options.h"

#include <QFileInfo>
#include <QTemporaryDir>

#include <optional>
#include <vector>

namespace {

std::optional<int64_t> insertTextFixture(bs::SQLiteStore& store,
                                         const QString& path,
                                         const QString& content,
                                         int64_t size,
                                         double modifiedAt)
{
    const QFileInfo info(path);
    const QString extension = info.suffix();
    auto itemId = store.upsertItem(path,
                                   info.fileName(),
                                   extension,
                                   bs::ItemKind::Text,
                                   size,
                                   /*createdAt=*/modifiedAt - 10.0,
                                   modifiedAt,
                                   QString(),
                                   QStringLiteral("normal"),
                                   info.path());
    if (!itemId.has_value()) {
        return std::nullopt;
    }

    bs::Chunk chunk;
    chunk.chunkId = bs::computeChunkId(path, 0);
    chunk.filePath = path;
    chunk.chunkIndex = 0;
    chunk.content = content;
    chunk.byteOffset = 0;
    const std::vector<bs::Chunk> chunks = {chunk};
    if (!store.insertChunks(itemId.value(), info.fileName(), path, chunks)) {
        return std::nullopt;
    }
    return itemId;
}

} // namespace

class TestSQLiteStoreExtended : public QObject {
    Q_OBJECT

private slots:
    void testFilteredFts5SearchOptions();
    void testFilteredNameSearchOptions();
    void testFeedbackAggregationBatchAndMaintenance();
};

void TestSQLiteStoreExtended::testFilteredFts5SearchOptions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + QStringLiteral("/filtered-search.db");

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString keyword = QStringLiteral("projectalpha");
    const auto passId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/pass.md"),
        keyword + QStringLiteral(" canonical matching record"),
        /*size=*/400,
        /*modifiedAt=*/220.0);
    const auto oldId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/old.md"),
        keyword + QStringLiteral(" old record"),
        /*size=*/400,
        /*modifiedAt=*/40.0);
    const auto outsideId = insertTextFixture(
        store,
        QStringLiteral("/workspace/other/outside.md"),
        keyword + QStringLiteral(" outside include roots"),
        /*size=*/400,
        /*modifiedAt=*/220.0);
    const auto wrongTypeId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/wrong.txt"),
        keyword + QStringLiteral(" wrong extension"),
        /*size=*/400,
        /*modifiedAt=*/220.0);
    const auto excludedId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/excluded/skip.md"),
        keyword + QStringLiteral(" explicitly excluded path"),
        /*size=*/400,
        /*modifiedAt=*/220.0);
    const auto tinyId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/tiny.md"),
        keyword + QStringLiteral(" tiny file"),
        /*size=*/2,
        /*modifiedAt=*/220.0);
    const auto hugeId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/huge.md"),
        keyword + QStringLiteral(" huge file"),
        /*size=*/200000,
        /*modifiedAt=*/220.0);

    QVERIFY(passId.has_value());
    QVERIFY(oldId.has_value());
    QVERIFY(outsideId.has_value());
    QVERIFY(wrongTypeId.has_value());
    QVERIFY(excludedId.has_value());
    QVERIFY(tinyId.has_value());
    QVERIFY(hugeId.has_value());

    bs::SearchOptions options;
    options.fileTypes = {QStringLiteral(".md")};
    options.includePaths = {QStringLiteral("/workspace/docs")};
    options.excludePaths = {QStringLiteral("/workspace/docs/excluded")};
    options.modifiedAfter = 100.0;
    options.modifiedBefore = 300.0;
    options.minSizeBytes = 10;
    options.maxSizeBytes = 10000;

    const auto hits = store.searchFts5(keyword, 20, /*relaxed=*/false, options);
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits.front().fileId, passId.value());
}

void TestSQLiteStoreExtended::testFilteredNameSearchOptions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + QStringLiteral("/filtered-name.db");

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const auto preferredId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/quarterly-report.md"),
        QStringLiteral("quarterly analysis"),
        /*size=*/300,
        /*modifiedAt=*/200.0);
    const auto wrongTypeId = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/quarterly-report.txt"),
        QStringLiteral("quarterly analysis"),
        /*size=*/300,
        /*modifiedAt=*/200.0);
    const auto outsideId = insertTextFixture(
        store,
        QStringLiteral("/workspace/other/quarterly-report.md"),
        QStringLiteral("quarterly analysis"),
        /*size=*/300,
        /*modifiedAt=*/200.0);

    QVERIFY(preferredId.has_value());
    QVERIFY(wrongTypeId.has_value());
    QVERIFY(outsideId.has_value());

    bs::SearchOptions options;
    options.fileTypes = {QStringLiteral("md")};
    options.includePaths = {QStringLiteral("/workspace/docs")};
    options.minSizeBytes = 100;
    options.maxSizeBytes = 1000;
    options.modifiedAfter = 150.0;
    options.modifiedBefore = 250.0;

    const auto hits = store.searchByNameFuzzy(QStringLiteral("quarterly report"), 10, options);
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits.front().fileId, preferredId.value());
}

void TestSQLiteStoreExtended::testFeedbackAggregationBatchAndMaintenance()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.path() + QStringLiteral("/feedback-maintenance.db");

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const auto idA = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/a.md"),
        QStringLiteral("alpha keyphrase"),
        /*size=*/220,
        /*modifiedAt=*/200.0);
    const auto idB = insertTextFixture(
        store,
        QStringLiteral("/workspace/docs/b.md"),
        QStringLiteral("beta keyphrase"),
        /*size=*/250,
        /*modifiedAt=*/200.0);
    QVERIFY(idA.has_value());
    QVERIFY(idB.has_value());

    QVERIFY(store.recordFeedback(idA.value(), QStringLiteral("opened"), QStringLiteral("alpha"), 1));
    QVERIFY(store.recordFeedback(idA.value(), QStringLiteral("opened"), QStringLiteral("alpha"), 2));
    QVERIFY(store.recordFeedback(idB.value(), QStringLiteral("opened"), QStringLiteral("beta"), 1));
    QVERIFY(store.incrementFrequency(idA.value()));

    const auto beforeBatch =
        store.getFrequenciesBatch({idA.value(), idB.value(), static_cast<int64_t>(999999)});
    QVERIFY(beforeBatch.find(idA.value()) != beforeBatch.end());
    QVERIFY(beforeBatch.find(idB.value()) == beforeBatch.end());

    QVERIFY(store.aggregateFeedback());

    const auto freqA = store.getFrequency(idA.value());
    const auto freqB = store.getFrequency(idB.value());
    QVERIFY(freqA.has_value());
    QVERIFY(freqB.has_value());
    QVERIFY(freqA->openCount >= 3);
    QCOMPARE(freqB->openCount, 1);

    QVERIFY(store.cleanupOldFeedback(-1));
    QVERIFY(store.aggregateFeedback());

    QVERIFY(store.optimizeFts5());
    QVERIFY(store.integrityCheck());
    QVERIFY(store.fts5IntegrityCheck());
    QVERIFY(store.walCheckpoint());
    QVERIFY(store.vacuum());
}

QTEST_MAIN(TestSQLiteStoreExtended)
#include "test_sqlite_store_extended.moc"
