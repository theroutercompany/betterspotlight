#include <QtTest/QtTest>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include "core/extraction/extraction_manager.h"

class TestExtractionOfficePdf : public QObject {
    Q_OBJECT

private slots:
    void testRtfExtractionViaTextutil();
    void testDocxExtractionViaTextutil();
};

void TestExtractionOfficePdf::testRtfExtractionViaTextutil()
{
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/textutil"))) {
        QSKIP("textutil is unavailable on this host");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString rtfPath = dir.path() + QStringLiteral("/sample.rtf");
    QFile rtfFile(rtfPath);
    QVERIFY(rtfFile.open(QIODevice::WriteOnly | QIODevice::Text));
    rtfFile.write("{\\rtf1\\ansi\\deff0 This is an RTF extraction test.}");
    rtfFile.close();

    bs::ExtractionManager manager;
    const auto result = manager.extract(rtfPath, bs::ItemKind::Text);
    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("RTF extraction test"),
                                     Qt::CaseInsensitive));
}

void TestExtractionOfficePdf::testDocxExtractionViaTextutil()
{
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/textutil"))) {
        QSKIP("textutil is unavailable on this host");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString txtPath = dir.path() + QStringLiteral("/seed.txt");
    QFile txtFile(txtPath);
    QVERIFY(txtFile.open(QIODevice::WriteOnly | QIODevice::Text));
    txtFile.write("This DOCX contains office extraction content.\n");
    txtFile.close();

    const QString docxPath = dir.path() + QStringLiteral("/sample.docx");
    QProcess convertProc;
    convertProc.start(QStringLiteral("/usr/bin/textutil"),
                      {QStringLiteral("-convert"), QStringLiteral("docx"),
                       QStringLiteral("-output"), docxPath, txtPath});
    QVERIFY(convertProc.waitForFinished(15000));
    if (convertProc.exitStatus() != QProcess::NormalExit || convertProc.exitCode() != 0) {
        QSKIP("textutil failed to generate docx fixture on this host");
    }

    bs::ExtractionManager manager;
    const auto result = manager.extract(docxPath, bs::ItemKind::Text);
    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("office extraction content"),
                                     Qt::CaseInsensitive));
}

QTEST_MAIN(TestExtractionOfficePdf)
#include "test_extraction_office_pdf.moc"
