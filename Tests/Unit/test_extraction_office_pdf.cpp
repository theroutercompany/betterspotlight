#include <QtTest/QtTest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>

#include "core/extraction/extraction_manager.h"

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* key, const QByteArray& value)
        : key_(key)
        , oldValue_(qgetenv(key))
        , hadValue_(!oldValue_.isNull())
    {
        qputenv(key_, value);
    }

    ~ScopedEnvVar()
    {
        if (hadValue_) {
            qputenv(key_, oldValue_);
        } else {
            qunsetenv(key_);
        }
    }

private:
    const char* key_ = nullptr;
    QByteArray oldValue_;
    bool hadValue_ = false;
};

QString writeExecutableScript(const QTemporaryDir& dir,
                              const QString& name,
                              const QString& body)
{
    const QString path = QDir(dir.path()).filePath(name);
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return QString();
    }
    script.write(body.toUtf8());
    script.close();

    if (!QFile::setPermissions(
            path,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                | QFileDevice::ReadOther | QFileDevice::ExeOther)) {
        return QString();
    }
    return path;
}

} // namespace

class TestExtractionOfficePdf : public QObject {
    Q_OBJECT

private slots:
    void testRtfExtractionViaTextutil();
    void testDocxExtractionViaTextutil();
    void testOfficeLikeTextFixtureFallsBackWhenMetadataEmpty();
    void testOfficeLikeBinaryDoesNotRawTextFallback();
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

void TestExtractionOfficePdf::testOfficeLikeTextFixtureFallsBackWhenMetadataEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString xlsxPath = QDir(dir.path()).filePath(QStringLiteral("budget-2026.xlsx"));
    QFile xlsxFile(xlsxPath);
    QVERIFY(xlsxFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    xlsxFile.write("# Mock XLSX Export\nRevenue,Expenses,Profit\n");
    xlsxFile.close();

    const QString mdimportOk = writeExecutableScript(
        dir,
        QStringLiteral("mdimport_ok.sh"),
        QStringLiteral("#!/bin/sh\nexit 0\n"));
    const QString mdlsEmpty = writeExecutableScript(
        dir,
        QStringLiteral("mdls_empty.sh"),
        QStringLiteral("#!/bin/sh\necho 'kMDItemTextContent = (null)'\nexit 0\n"));
    QVERIFY(!mdimportOk.isEmpty());
    QVERIFY(!mdlsEmpty.isEmpty());

    ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
    ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsEmpty.toUtf8());

    bs::ExtractionManager manager;
    const auto result = manager.extract(xlsxPath, bs::ItemKind::Text);

    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("Revenue,Expenses,Profit")));
}

void TestExtractionOfficePdf::testOfficeLikeBinaryDoesNotRawTextFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString pptxPath = QDir(dir.path()).filePath(QStringLiteral("slides.pptx"));
    QFile pptxFile(pptxPath);
    QVERIFY(pptxFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    pptxFile.write(QByteArray("PK\003\004\0\0\1\2\3binary office payload", 30));
    pptxFile.close();

    const QString mdimportOk = writeExecutableScript(
        dir,
        QStringLiteral("mdimport_ok.sh"),
        QStringLiteral("#!/bin/sh\nexit 0\n"));
    const QString mdlsEmpty = writeExecutableScript(
        dir,
        QStringLiteral("mdls_empty.sh"),
        QStringLiteral("#!/bin/sh\necho 'kMDItemTextContent = (null)'\nexit 0\n"));
    QVERIFY(!mdimportOk.isEmpty());
    QVERIFY(!mdlsEmpty.isEmpty());

    ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
    ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsEmpty.toUtf8());

    bs::ExtractionManager manager;
    const auto result = manager.extract(pptxPath, bs::ItemKind::Text);

    QCOMPARE(result.status, bs::ExtractionResult::Status::UnsupportedFormat);
    QVERIFY(result.errorMessage.has_value());
    QVERIFY(result.errorMessage->contains(QStringLiteral("empty")));
}

QTEST_MAIN(TestExtractionOfficePdf)
#include "test_extraction_office_pdf.moc"
