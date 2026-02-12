#include <QtTest/QtTest>

#include "core/extraction/pdf_extractor.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

class TestPdfExtractor : public QObject {
    Q_OBJECT

private slots:
    void testSupportsAndMissingPath();
    void testCorruptedAndValidPdf();
};

void TestPdfExtractor::testSupportsAndMissingPath()
{
    bs::PdfExtractor extractor;

    QVERIFY(extractor.supports(QStringLiteral("pdf")));
    QVERIFY(extractor.supports(QStringLiteral("PDF")));
    QVERIFY(!extractor.supports(QStringLiteral("txt")));

    const bs::ExtractionResult missing = extractor.extract(QStringLiteral("/no/such/file.pdf"));
    QCOMPARE(missing.status, bs::ExtractionResult::Status::Inaccessible);
    QVERIFY(missing.errorMessage.has_value());
}

void TestPdfExtractor::testCorruptedAndValidPdf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    bs::PdfExtractor extractor;

    const QString brokenPdfPath = dir.path() + QStringLiteral("/broken.pdf");
    {
        QFile broken(brokenPdfPath);
        QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        broken.write("not a pdf file");
    }

    const bs::ExtractionResult broken = extractor.extract(brokenPdfPath);
    QVERIFY(broken.status == bs::ExtractionResult::Status::CorruptedFile
            || broken.status == bs::ExtractionResult::Status::UnsupportedFormat);
    if (broken.status == bs::ExtractionResult::Status::UnsupportedFormat) {
        return;
    }

    if (!QFileInfo::exists(QStringLiteral("/usr/bin/textutil"))) {
        QSKIP("textutil is unavailable on this host");
    }

    const QString seedPath = dir.path() + QStringLiteral("/seed.txt");
    {
        QFile seed(seedPath);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        seed.write("PDF extractor integration fixture content");
    }

    const QString pdfPath = dir.path() + QStringLiteral("/fixture.pdf");
    QProcess convert;
    convert.start(QStringLiteral("/usr/bin/textutil"),
                  {QStringLiteral("-convert"), QStringLiteral("pdf"),
                   QStringLiteral("-output"), pdfPath, seedPath});
    QVERIFY(convert.waitForFinished(15000));
    if (convert.exitStatus() != QProcess::NormalExit || convert.exitCode() != 0) {
        QSKIP("textutil failed to generate PDF fixture");
    }

    const bs::ExtractionResult ok = extractor.extract(pdfPath);
    QCOMPARE(ok.status, bs::ExtractionResult::Status::Success);
    QVERIFY(ok.content.has_value());
    QVERIFY(ok.content->contains(QStringLiteral("extractor integration fixture"),
                                 Qt::CaseInsensitive));
}

QTEST_MAIN(TestPdfExtractor)
#include "test_pdf_extractor.moc"
