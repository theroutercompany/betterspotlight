#include <QtTest/QtTest>

#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"
#include "core/index/sqlite_store.h"
#include "core/indexing/chunker.h"
#include "core/indexing/indexer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

bool writeTextFile(const QString& path, const QByteArray& payload)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(payload) != payload.size()) {
        return false;
    }
    file.close();
    return true;
}

} // namespace

class TestIndexer : public QObject {
    Q_OBJECT

private slots:
    void testExcludeAndDeleteLifecycle();
    void testMetadataOnlyRescanAndSkipBranches();
    void testNonExtractableAndExtractionFailurePaths();
};

void TestIndexer::testExcludeAndDeleteLifecycle()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(store, extractor, pathRules, chunker);

    const QString excludedDir = QDir(tempDir.path()).filePath(QStringLiteral("node_modules/pkg"));
    QVERIFY(QDir().mkpath(excludedDir));
    const QString excludedPath = QDir(excludedDir).filePath(QStringLiteral("index.js"));
    QVERIFY(writeTextFile(excludedPath, QByteArrayLiteral("module.exports = 1;\n")));

    bs::WorkItem excludedWork;
    excludedWork.type = bs::WorkItem::Type::NewFile;
    excludedWork.filePath = excludedPath.toStdString();

    const bs::PreparedWork excludedPrepared = indexer.prepareWorkItem(excludedWork, 99);
    QCOMPARE(excludedPrepared.validation, bs::ValidationResult::Exclude);
    QCOMPARE(excludedPrepared.generation, static_cast<uint64_t>(99));
    QCOMPARE(excludedPrepared.retryCount, 0);

    const bs::IndexResult excludedResult = indexer.applyPreparedWork(excludedPrepared);
    QCOMPARE(excludedResult.status, bs::IndexResult::Status::Excluded);
    QVERIFY(!store.getItemByPath(excludedPath).has_value());

    const QString indexedPath = QDir(tempDir.path()).filePath(QStringLiteral("keep.txt"));
    QVERIFY(writeTextFile(indexedPath, QByteArrayLiteral("alpha beta gamma delta")));

    bs::WorkItem newFile;
    newFile.type = bs::WorkItem::Type::NewFile;
    newFile.filePath = indexedPath.toStdString();
    const bs::IndexResult indexed = indexer.processWorkItem(newFile);
    QCOMPARE(indexed.status, bs::IndexResult::Status::Indexed);
    QVERIFY(indexed.chunksInserted > 0);
    QVERIFY(store.getItemByPath(indexedPath).has_value());

    bs::WorkItem deleteExisting;
    deleteExisting.type = bs::WorkItem::Type::Delete;
    deleteExisting.filePath = indexedPath.toStdString();
    const bs::PreparedWork deletePrepared = indexer.prepareWorkItem(deleteExisting, 5);
    QCOMPARE(deletePrepared.validation, bs::ValidationResult::Include);
    const bs::IndexResult deleted = indexer.applyPreparedWork(deletePrepared);
    QCOMPARE(deleted.status, bs::IndexResult::Status::Deleted);
    QVERIFY(!store.getItemByPath(indexedPath).has_value());

    bs::WorkItem deleteMissing;
    deleteMissing.type = bs::WorkItem::Type::Delete;
    deleteMissing.filePath = QDir(tempDir.path()).filePath(QStringLiteral("missing.txt")).toStdString();
    const bs::IndexResult missingDelete = indexer.processWorkItem(deleteMissing);
    QCOMPARE(missingDelete.status, bs::IndexResult::Status::Deleted);
}

void TestIndexer::testMetadataOnlyRescanAndSkipBranches()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(store, extractor, pathRules, chunker);

    const QString sensitiveDir = QDir(tempDir.path()).filePath(QStringLiteral(".ssh"));
    QVERIFY(QDir().mkpath(sensitiveDir));
    const QString sensitivePath = QDir(sensitiveDir).filePath(QStringLiteral("id_rsa"));
    QVERIFY(writeTextFile(sensitivePath, QByteArrayLiteral("PRIVATE-KEY-MATERIAL")));

    bs::WorkItem sensitiveItem;
    sensitiveItem.type = bs::WorkItem::Type::NewFile;
    sensitiveItem.filePath = sensitivePath.toStdString();
    const bs::PreparedWork sensitivePrepared = indexer.prepareWorkItem(sensitiveItem, 7);
    QCOMPARE(sensitivePrepared.validation, bs::ValidationResult::MetadataOnly);
    QVERIFY(sensitivePrepared.metadata.has_value());
    const bs::IndexResult sensitiveResult = indexer.applyPreparedWork(sensitivePrepared);
    QCOMPARE(sensitiveResult.status, bs::IndexResult::Status::MetadataOnly);
    QVERIFY(store.getItemByPath(sensitivePath).has_value());

    const QString rescannedDir = QDir(tempDir.path()).filePath(QStringLiteral("rescanned"));
    QVERIFY(QDir().mkpath(rescannedDir));
    bs::WorkItem rescanItem;
    rescanItem.type = bs::WorkItem::Type::RescanDirectory;
    rescanItem.filePath = rescannedDir.toStdString();
    const bs::PreparedWork rescanPrepared = indexer.prepareWorkItem(rescanItem, 9);
    QVERIFY(rescanPrepared.metadata.has_value());
    QCOMPARE(rescanPrepared.type, bs::WorkItem::Type::RescanDirectory);
    const bs::IndexResult rescanResult = indexer.applyPreparedWork(rescanPrepared);
    QCOMPARE(rescanResult.status, bs::IndexResult::Status::Indexed);

    const QString normalPath = QDir(tempDir.path()).filePath(QStringLiteral("stable.txt"));
    const QByteArray stablePayload("same-content-across-modifications");
    QVERIFY(writeTextFile(normalPath, stablePayload));

    bs::WorkItem firstIndex;
    firstIndex.type = bs::WorkItem::Type::NewFile;
    firstIndex.filePath = normalPath.toStdString();
    QCOMPARE(indexer.processWorkItem(firstIndex).status, bs::IndexResult::Status::Indexed);

    bs::WorkItem unchangedMod;
    unchangedMod.type = bs::WorkItem::Type::ModifiedContent;
    unchangedMod.filePath = normalPath.toStdString();
    const bs::IndexResult unchangedResult = indexer.processWorkItem(unchangedMod);
    QCOMPARE(unchangedResult.status, bs::IndexResult::Status::Skipped);

    QTest::qSleep(1200);
    QVERIFY(writeTextFile(normalPath, stablePayload));

    bs::WorkItem hashEquivalentMod;
    hashEquivalentMod.type = bs::WorkItem::Type::ModifiedContent;
    hashEquivalentMod.filePath = normalPath.toStdString();
    const bs::IndexResult hashEquivalentResult = indexer.processWorkItem(hashEquivalentMod);
    QCOMPARE(hashEquivalentResult.status, bs::IndexResult::Status::Skipped);
}

void TestIndexer::testNonExtractableAndExtractionFailurePaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(store, extractor, pathRules, chunker);

    const QString unknownPath = QDir(tempDir.path()).filePath(QStringLiteral("payload.weird"));
    {
        QFile file(unknownPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray binary("\x00\x01\x02\x03\x04\xff", 6);
        QCOMPARE(file.write(binary), binary.size());
        file.close();
    }

    bs::WorkItem unknownItem;
    unknownItem.type = bs::WorkItem::Type::NewFile;
    unknownItem.filePath = unknownPath.toStdString();
    const bs::IndexResult unknownResult = indexer.processWorkItem(unknownItem);
    QCOMPARE(unknownResult.status, bs::IndexResult::Status::Indexed);

    const auto unknownRow = store.getItemByPath(unknownPath);
    QVERIFY(unknownRow.has_value());
    const auto unknownAvailability = store.getItemAvailability(unknownRow->id);
    QVERIFY(unknownAvailability.has_value());
    QVERIFY(!unknownAvailability->contentAvailable);

    extractor.setMaxFileSizeBytes(1);
    const QString oversizedPath = QDir(tempDir.path()).filePath(QStringLiteral("oversized.txt"));
    QVERIFY(writeTextFile(oversizedPath, QByteArrayLiteral("abcdef")));

    bs::WorkItem oversizedItem;
    oversizedItem.type = bs::WorkItem::Type::NewFile;
    oversizedItem.filePath = oversizedPath.toStdString();
    const bs::IndexResult oversizedResult = indexer.processWorkItem(oversizedItem);
    QCOMPARE(oversizedResult.status, bs::IndexResult::Status::ExtractionFailed);

    const auto oversizedRow = store.getItemByPath(oversizedPath);
    QVERIFY(oversizedRow.has_value());
    const auto oversizedAvailability = store.getItemAvailability(oversizedRow->id);
    QVERIFY(oversizedAvailability.has_value());
    QVERIFY(!oversizedAvailability->lastExtractionError.isEmpty());
    QVERIFY(oversizedAvailability->lastExtractionError.contains(
        QStringLiteral("exceeds configured limit"), Qt::CaseInsensitive));
    QCOMPARE(oversizedAvailability->availabilityStatus, QStringLiteral("extract_failed"));

    extractor.setMaxFileSizeBytes(50LL * 1024LL * 1024LL);
    const QString manualPath = QDir(tempDir.path()).filePath(QStringLiteral("manual.txt"));
    QVERIFY(writeTextFile(manualPath, QByteArrayLiteral("manual branch coverage text")));

    bs::WorkItem manualItem;
    manualItem.type = bs::WorkItem::Type::NewFile;
    manualItem.filePath = manualPath.toStdString();
    bs::PreparedWork manualPrepared = indexer.prepareWorkItem(manualItem, 11);
    QVERIFY(manualPrepared.metadata.has_value());
    QVERIFY(manualPrepared.hasExtractedContent);
    manualPrepared.hasExtractedContent = false;
    manualPrepared.nonExtractable = false;
    manualPrepared.failure.reset();
    manualPrepared.chunks.clear();

    const bs::IndexResult manualResult = indexer.applyPreparedWork(manualPrepared);
    QCOMPARE(manualResult.status, bs::IndexResult::Status::ExtractionFailed);
}

QTEST_MAIN(TestIndexer)
#include "test_indexer.moc"
