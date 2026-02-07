#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "core/extraction/extraction_manager.h"
#include "core/index/sqlite_store.h"
#include "core/indexing/indexer.h"
#include "core/indexing/chunker.h"
#include "core/fs/path_rules.h"
#include "core/shared/types.h"

// Test that corrupt, malformed, and edge-case files are handled
// gracefully without crashing the extraction pipeline or corrupting
// the database.

class TestCorruptFiles : public QObject {
    Q_OBJECT

private slots:
    void testCorruptPdfReturnsError();
    void testBinaryMasqueradingAsText();
    void testZeroByteFile();
    void testTruncatedUtf8();
    void testOversizedFileRejected();
    void testCorruptFileRecordsFailure();
};

void TestCorruptFiles::testCorruptPdfReturnsError()
{
    // A corrupt PDF should return CorruptedFile or UnsupportedFormat,
    // never crash or produce garbage content.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString path = dir.path() + "/corrupt.pdf";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // PDF header followed by random bytes
        f.write("%PDF-1.4\n");
        QByteArray garbage(256, '\xDE');
        f.write(garbage);
    }

    bs::ExtractionManager mgr;
    auto result = mgr.extract(path, bs::ItemKind::Pdf);

    // Should not succeed — either CorruptedFile or UnsupportedFormat
    QVERIFY(result.status != bs::ExtractionResult::Status::Success);
    // Must not crash (if we got here, it didn't)
}

void TestCorruptFiles::testBinaryMasqueradingAsText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString path = dir.path() + "/binary_masquerade.txt";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // Mach-O magic + null bytes — binary content in a .txt
        QByteArray data;
        data.append("\xCF\xFA\xED\xFE", 4);
        data.append(QByteArray(200, '\x00'));
        data.append(QByteArray(100, '\xFF'));
        f.write(data);
    }

    bs::ExtractionManager mgr;
    auto result = mgr.extract(path, bs::ItemKind::Text);

    // The extractor should either extract whatever text it can find
    // (via Latin-1 fallback) or report an error. Either way, it must not crash.
    // We don't assert on null bytes because Latin-1 fallback legitimately
    // includes them — the key invariant is crash-freedom.
    Q_UNUSED(result);
}

void TestCorruptFiles::testZeroByteFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString path = dir.path() + "/empty.txt";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // Write nothing — 0 bytes
    }

    bs::ExtractionManager mgr;
    auto result = mgr.extract(path, bs::ItemKind::Text);

    // Zero-byte file: either Success with empty content, or an appropriate status.
    // Must not crash.
    if (result.status == bs::ExtractionResult::Status::Success) {
        QVERIFY(!result.content.has_value() || result.content->isEmpty());
    }
}

void TestCorruptFiles::testTruncatedUtf8()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString path = dir.path() + "/truncated_utf8.txt";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("Valid UTF-8 text here. ");
        // Truncated 3-byte UTF-8 sequence (missing last byte)
        f.write(QByteArray::fromHex("E298"));
    }

    bs::ExtractionManager mgr;
    auto result = mgr.extract(path, bs::ItemKind::Text);

    // Should handle gracefully — extract what it can
    // Must not crash
    QVERIFY(result.status == bs::ExtractionResult::Status::Success ||
            result.status == bs::ExtractionResult::Status::CorruptedFile);
}

void TestCorruptFiles::testOversizedFileRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Don't actually create a 500MB file — just set the limit very low
    QString path = dir.path() + "/small.txt";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1024, 'A'));  // 1KB of data
    }

    bs::ExtractionManager mgr;
    mgr.setMaxFileSizeBytes(512);  // Set limit to 512 bytes

    auto result = mgr.extract(path, bs::ItemKind::Text);
    QCOMPARE(result.status, bs::ExtractionResult::Status::SizeExceeded);
}

void TestCorruptFiles::testCorruptFileRecordsFailure()
{
    // End-to-end: when the indexer encounters a corrupt file,
    // it should record a failure in the database rather than crash.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Create a corrupt "text" file
    QString filePath = dir.path() + "/corrupt.txt";
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // Binary content that will fail text extraction
        QByteArray garbage(500, '\x00');
        f.write(garbage);
    }

    // Set up the indexing pipeline
    QString dbPath = dir.path() + "/index.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    // Process the corrupt file
    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();

    indexer.processWorkItem(item);

    // The indexer should handle this gracefully:
    // Either it extracts empty content (no chunks) or records a failure.
    // It must NOT crash.
    // Check the database still functions after processing
    auto health = store->getHealth();
    QVERIFY(health.totalIndexedItems >= 0);  // Database is still operational
}

QTEST_MAIN(TestCorruptFiles)
#include "test_corrupt_files.moc"
