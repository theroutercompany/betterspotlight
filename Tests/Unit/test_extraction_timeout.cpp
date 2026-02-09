#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "core/extraction/extraction_manager.h"

class TestExtractionTimeout : public QObject {
    Q_OBJECT

private slots:
    void testDeadlineConstant();
    void testExtractionCompletesWithinTimeout();
    void testCancelledExtractionReturnsImmediately();
    void testMaxFileSizeEnforcement();
};

void TestExtractionTimeout::testDeadlineConstant()
{
    // Verify the deadline constant is set to 30 seconds
    QCOMPARE(bs::ExtractionManager::kMaxExtractionMs, 30000);
}

void TestExtractionTimeout::testExtractionCompletesWithinTimeout()
{
    bs::ExtractionManager mgr;

    // Create a small text file
    QTemporaryFile tmpFile;
    tmpFile.setAutoRemove(true);
    QVERIFY(tmpFile.open());
    tmpFile.write("Hello world, this is a test file for extraction.\n");
    tmpFile.flush();

    QElapsedTimer timer;
    timer.start();

    bs::ExtractionResult result = mgr.extract(tmpFile.fileName(), bs::ItemKind::Text);

    // Extraction should complete well within the 30s deadline for a small file
    QVERIFY(timer.elapsed() < 5000);

    // Result should be success or unsupported (depending on file extension)
    QVERIFY(result.status == bs::ExtractionResult::Status::Success
            || result.status == bs::ExtractionResult::Status::UnsupportedFormat);
}

void TestExtractionTimeout::testCancelledExtractionReturnsImmediately()
{
    bs::ExtractionManager mgr;
    mgr.requestCancel();

    QTemporaryFile tmpFile;
    tmpFile.setAutoRemove(true);
    QVERIFY(tmpFile.open());
    tmpFile.write("Some content\n");
    tmpFile.flush();

    QElapsedTimer timer;
    timer.start();

    bs::ExtractionResult result = mgr.extract(tmpFile.fileName(), bs::ItemKind::Text);

    // Cancelled extraction should return very quickly
    QVERIFY(timer.elapsed() < 1000);
    QCOMPARE(result.status, bs::ExtractionResult::Status::Cancelled);

    mgr.clearCancel();
}

void TestExtractionTimeout::testMaxFileSizeEnforcement()
{
    bs::ExtractionManager mgr;
    mgr.setMaxFileSizeBytes(10);  // Very small limit

    QTemporaryFile tmpFile;
    tmpFile.setAutoRemove(true);
    QVERIFY(tmpFile.open());
    tmpFile.write("This content exceeds the 10-byte limit easily\n");
    tmpFile.flush();

    bs::ExtractionResult result = mgr.extract(tmpFile.fileName(), bs::ItemKind::Text);

    QCOMPARE(result.status, bs::ExtractionResult::Status::SizeExceeded);
}

QTEST_MAIN(TestExtractionTimeout)
#include "test_extraction_timeout.moc"
