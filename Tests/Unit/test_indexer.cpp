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
#include <QVector>

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

QByteArray escapePdfLiteral(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    QByteArray escaped;
    escaped.reserve(utf8.size() * 2);
    for (char ch : utf8) {
        if (ch == '\\' || ch == '(' || ch == ')') {
            escaped.append('\\');
        }
        escaped.append(ch);
    }
    return escaped;
}

QByteArray buildSinglePagePdf(const QString& text)
{
    const QByteArray literal = escapePdfLiteral(text);
    const QByteArray contentStream =
        "BT\n"
        "/F1 18 Tf\n"
        "72 720 Td\n"
        "(" + literal + ") Tj\n"
        "ET\n";

    QVector<QByteArray> objects;
    objects.append("<< /Type /Catalog /Pages 2 0 R >>");
    objects.append("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                   "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>");
    objects.append("<< /Length " + QByteArray::number(contentStream.size())
                   + " >>\nstream\n" + contentStream + "endstream");
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    QByteArray pdf = "%PDF-1.4\n";
    QVector<int> offsets;
    offsets.reserve(objects.size());

    for (int i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n";
        pdf += objects.at(i);
        pdf += "\nendobj\n";
    }

    const int xrefOffset = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0');
        pdf += " 00000 n \n";
    }

    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\n";
    pdf += "startxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

} // namespace

class TestIndexer : public QObject {
    Q_OBJECT

private slots:
    void testExcludeAndDeleteLifecycle();
    void testMetadataOnlyRescanAndSkipBranches();
    void testNonExtractableAndExtractionFailurePaths();
    void testPdfIndexingRequiresSupportedCapability();
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
    QVERIFY(unknownAvailability->lastExtractionError.isEmpty());
    QVERIFY(unknownAvailability->availabilityStatus != QStringLiteral("extract_failed"));

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

void TestIndexer::testPdfIndexingRequiresSupportedCapability()
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

    const QString pdfPath = QDir(tempDir.path()).filePath(QStringLiteral("supported.pdf"));
    QFile pdfFile(pdfPath);
    QVERIFY(pdfFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray payload = buildSinglePagePdf(QStringLiteral("Indexer PDF supported profile"));
    QVERIFY(pdfFile.write(payload) == payload.size());
    pdfFile.close();

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = pdfPath.toStdString();
    const bs::IndexResult result = indexer.processWorkItem(item);

    const QString pdfCapability =
        qEnvironmentVariable("BS_DEV_PDF_CAPABILITY").trimmed().toLower();
    const bool supportedPdfCapability = (pdfCapability == QStringLiteral("ready"));

    if (!supportedPdfCapability
        && result.status == bs::IndexResult::Status::Indexed
        && result.chunksInserted == 0) {
        QSKIP("PDF backend unavailable on this host");
    }

    if (result.status == bs::IndexResult::Status::ExtractionFailed) {
        if (supportedPdfCapability) {
            QFAIL("PDF indexing failed while BS_DEV_PDF_CAPABILITY=ready");
        }
        QSKIP("PDF backend unavailable on this host");
    }

    QCOMPARE(result.status, bs::IndexResult::Status::Indexed);
    QVERIFY(result.chunksInserted > 0);

    const auto row = store.getItemByPath(pdfPath);
    QVERIFY(row.has_value());
    const auto availability = store.getItemAvailability(row->id);
    QVERIFY(availability.has_value());
    QVERIFY(availability->contentAvailable);
    QCOMPARE(availability->availabilityStatus, QStringLiteral("available"));
}

QTEST_MAIN(TestIndexer)
#include "test_indexer.moc"
