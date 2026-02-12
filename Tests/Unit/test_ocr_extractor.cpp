#include <QtTest/QtTest>

#include "core/extraction/ocr_extractor.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

class TestOcrExtractor : public QObject {
    Q_OBJECT

private slots:
    void testSupportsAndMissingPath();
    void testUnreadableAndAccessiblePaths();
    void testMoveSemanticsPreserveExtractorUsage();
    void testValidImageExercisesOcrPath();
};

void TestOcrExtractor::testSupportsAndMissingPath()
{
    bs::OcrExtractor extractor;

    QVERIFY(extractor.supports(QStringLiteral("png")));
    QVERIFY(extractor.supports(QStringLiteral("JPEG")));
    QVERIFY(extractor.supports(QStringLiteral("tif")));
    QVERIFY(!extractor.supports(QStringLiteral("txt")));

    const bs::ExtractionResult missing =
        extractor.extract(QStringLiteral("/definitely/missing/image.png"));
    QCOMPARE(missing.status, bs::ExtractionResult::Status::Inaccessible);
    QVERIFY(missing.errorMessage.has_value());
}

void TestOcrExtractor::testUnreadableAndAccessiblePaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    bs::OcrExtractor extractor;

    const QString unreadablePath = dir.path() + QStringLiteral("/blocked.png");
    {
        QFile file(unreadablePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not-an-image");
        file.close();
    }

    QVERIFY(QFile::setPermissions(unreadablePath, QFileDevice::WriteOwner));
    const bs::ExtractionResult unreadable = extractor.extract(unreadablePath);
    if (unreadable.status != bs::ExtractionResult::Status::Inaccessible) {
        QSKIP("Unable to produce an unreadable file on this host");
    }
    QVERIFY(unreadable.errorMessage.has_value());

    QVERIFY(QFile::setPermissions(unreadablePath,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    const QString readablePath = dir.path() + QStringLiteral("/sample.png");
    {
        QFile file(readablePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not-a-real-png-payload");
        file.close();
    }

    const bs::ExtractionResult readable = extractor.extract(readablePath);
    QVERIFY(readable.status == bs::ExtractionResult::Status::UnsupportedFormat
            || readable.status == bs::ExtractionResult::Status::CorruptedFile);
    QVERIFY(readable.durationMs >= 0);
}

void TestOcrExtractor::testMoveSemanticsPreserveExtractorUsage()
{
    bs::OcrExtractor first;
    bs::OcrExtractor moved(std::move(first));
    QVERIFY(moved.supports(QStringLiteral("png")));

    bs::OcrExtractor assigned;
    assigned = std::move(moved);
    QVERIFY(assigned.supports(QStringLiteral("jpeg")));
}

void TestOcrExtractor::testValidImageExercisesOcrPath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString pngPath = dir.path() + QStringLiteral("/tiny.png");
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::white);
    image.setPixelColor(8, 8, Qt::black);
    QVERIFY(image.save(pngPath, "PNG"));

    bs::OcrExtractor extractor;
    const bs::ExtractionResult result = extractor.extract(pngPath);

    if (result.status == bs::ExtractionResult::Status::UnsupportedFormat) {
        QVERIFY(result.errorMessage.has_value());
        const bool unavailable = result.errorMessage->contains(QStringLiteral("initialise"),
                                                                Qt::CaseInsensitive)
            || result.errorMessage->contains(QStringLiteral("unavailable"),
                                             Qt::CaseInsensitive);
        QVERIFY(unavailable);
        QSKIP("OCR backend unavailable on this host");
    }
    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
}

QTEST_MAIN(TestOcrExtractor)
#include "test_ocr_extractor.moc"
