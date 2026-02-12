#include <QtTest/QtTest>

#include "core/extraction/pdf_extractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QVector>

namespace {

QString fixturePdfPath()
{
#ifdef BETTERSPOTLIGHT_SOURCE_DIR
    return QDir(QString::fromUtf8(BETTERSPOTLIGHT_SOURCE_DIR))
        .filePath(QStringLiteral("Tests/Fixtures/standard_home_v1/Downloads/invoice-january-2026.pdf"));
#else
    return QString();
#endif
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

class TestPdfExtractor : public QObject {
    Q_OBJECT

private slots:
    void testSupportsAndMissingPath();
    void testExtractsProgrammaticValidPdf();
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

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString directoryPath = dir.path() + QStringLiteral("/as-directory.pdf");
    QVERIFY(QDir().mkpath(directoryPath));
    const bs::ExtractionResult directoryResult = extractor.extract(directoryPath);
    QCOMPARE(directoryResult.status, bs::ExtractionResult::Status::Inaccessible);

    const QString unreadablePath = dir.path() + QStringLiteral("/unreadable.pdf");
    {
        QFile unreadable(unreadablePath);
        QVERIFY(unreadable.open(QIODevice::WriteOnly | QIODevice::Truncate));
        unreadable.write("placeholder");
        unreadable.close();
    }
    QVERIFY(QFile::setPermissions(unreadablePath, QFileDevice::WriteOwner));
    const bs::ExtractionResult unreadableResult = extractor.extract(unreadablePath);
    if (unreadableResult.status == bs::ExtractionResult::Status::Inaccessible) {
        QVERIFY(unreadableResult.errorMessage.has_value());
        QVERIFY(unreadableResult.errorMessage->contains(QStringLiteral("readable"),
                                                        Qt::CaseInsensitive));
    } else {
        QSKIP("Unable to produce unreadable file on this host");
    }
    QVERIFY(QFile::setPermissions(unreadablePath,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

void TestPdfExtractor::testExtractsProgrammaticValidPdf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString pdfPath = dir.path() + QStringLiteral("/generated.pdf");
    QFile file(pdfPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray pdfData = buildSinglePagePdf(QStringLiteral("BetterSpotlight extractor contract"));
    QVERIFY(file.write(pdfData) == pdfData.size());
    file.close();

    bs::PdfExtractor extractor;
    const bs::ExtractionResult result = extractor.extract(pdfPath);

    if (result.status == bs::ExtractionResult::Status::UnsupportedFormat) {
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage->contains(QStringLiteral("unavailable"),
                                              Qt::CaseInsensitive));
        QSKIP("PDF backend unavailable on this host");
    }

    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("BetterSpotlight"),
                                     Qt::CaseInsensitive));
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

    const QString fixturePath = fixturePdfPath();
    if (!fixturePath.isEmpty() && QFileInfo::exists(fixturePath)) {
        const bs::ExtractionResult fixture = extractor.extract(fixturePath);
        QVERIFY(fixture.status != bs::ExtractionResult::Status::Inaccessible);
        if (fixture.status == bs::ExtractionResult::Status::Success) {
            QVERIFY(fixture.content.has_value());
            QVERIFY(!fixture.content->trimmed().isEmpty());
        }
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
