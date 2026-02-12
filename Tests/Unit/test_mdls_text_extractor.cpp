#include <QtTest/QtTest>

#include "core/extraction/mdls_text_extractor.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStringView>
#include <QTemporaryDir>

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

QString writeExecutableScript(const QTemporaryDir& dir, QStringView fileName, QStringView scriptBody)
{
    const QString scriptPath = QDir(dir.path()).filePath(fileName.toString());
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return QString();
    }
    script.write(scriptBody.toUtf8());
    script.close();

    if (!QFile::setPermissions(
            scriptPath,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                | QFileDevice::ReadOther | QFileDevice::ExeOther)) {
        return QString();
    }
    return scriptPath;
}

QString createFixtureFile(const QTemporaryDir& dir, QStringView fileName, const QByteArray& bytes)
{
    const QString filePath = QDir(dir.path()).filePath(fileName.toString());
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(bytes);
    file.close();
    return filePath;
}

} // namespace

class TestMdlsTextExtractor : public QObject {
    Q_OBJECT

private slots:
    void testSupportsExpectedExtensions();
    void testExtractRejectsMissingPath();
    void testExtractRejectsDirectory();
    void testExtractRejectsOversizedFile();

private:
    void runProcessScenarios();
};

void TestMdlsTextExtractor::testSupportsExpectedExtensions()
{
    bs::MdlsTextExtractor extractor;
    QVERIFY(extractor.supports(QStringLiteral("xlsx")));
    QVERIFY(extractor.supports(QStringLiteral("PAGES")));
    QVERIFY(!extractor.supports(QStringLiteral("txt")));
}

void TestMdlsTextExtractor::testExtractRejectsMissingPath()
{
    bs::MdlsTextExtractor extractor;
    const bs::ExtractionResult result = extractor.extract(QStringLiteral("/no/such/file.pages"));
    QCOMPARE(result.status, bs::ExtractionResult::Status::Inaccessible);
    QVERIFY(result.errorMessage.has_value());
}

void TestMdlsTextExtractor::testExtractRejectsDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    bs::MdlsTextExtractor extractor;
    const bs::ExtractionResult result = extractor.extract(dir.path());
    QCOMPARE(result.status, bs::ExtractionResult::Status::Inaccessible);
}

void TestMdlsTextExtractor::testExtractRejectsOversizedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString filePath = QDir(dir.path()).filePath(QStringLiteral("huge.pages"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.resize(51 * 1024 * 1024));
    file.close();

    bs::MdlsTextExtractor extractor;
    const bs::ExtractionResult result = extractor.extract(filePath);
    QCOMPARE(result.status, bs::ExtractionResult::Status::SizeExceeded);
    QVERIFY(result.errorMessage.has_value());

    runProcessScenarios();
}

void TestMdlsTextExtractor::runProcessScenarios()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString unreadablePath =
        createFixtureFile(dir, QStringLiteral("unreadable.pages"), QByteArray("placeholder"));
    QVERIFY(!unreadablePath.isEmpty());
    QVERIFY(QFile::setPermissions(unreadablePath, QFileDevice::WriteOwner));
    {
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(unreadablePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Inaccessible);
        QVERIFY(result.errorMessage.has_value());
    }

    const QString filePath = createFixtureFile(dir, QStringLiteral("fixture.pages"), QByteArray("fixture"));
    QVERIFY(!filePath.isEmpty());

    const QString mdimportOk = writeExecutableScript(
        dir,
        QStringLiteral("mdimport_ok.sh"),
        QStringLiteral("#!/bin/sh\nexit 0\n"));
    const QString mdlsQuoted = writeExecutableScript(
        dir,
        QStringLiteral("mdls_quoted.sh"),
        QStringLiteral(
            "#!/bin/sh\n"
            "echo 'kMDItemTextContent = (\"alpha\\\\nline\", \"tab\\\\tvalue\", \"slash\\\\\\\\ok\")'\n"
            "exit 0\n"));
    QVERIFY(!mdimportOk.isEmpty());
    QVERIFY(!mdlsQuoted.isEmpty());

    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsQuoted.toUtf8());
        ScopedEnvVar timeout("BS_TEST_MDLS_TIMEOUT_MS", "500");

        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
        QVERIFY(result.content.has_value());
        QCOMPARE(result.content.value(), QStringLiteral("alpha\nline\ntab\tvalue\nslash\\ok"));
    }

    const QString mdlsRaw = writeExecutableScript(
        dir,
        QStringLiteral("mdls_raw.sh"),
        QStringLiteral("#!/bin/sh\necho 'kMDItemTextContent = plain body text'\nexit 0\n"));
    QVERIFY(!mdlsRaw.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsRaw.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
        QVERIFY(result.content.has_value());
        QCOMPARE(result.content.value(), QStringLiteral("plain body text"));
    }

    const QString mdlsNull = writeExecutableScript(
        dir,
        QStringLiteral("mdls_null.sh"),
        QStringLiteral("#!/bin/sh\necho 'kMDItemTextContent = (null)'\nexit 0\n"));
    QVERIFY(!mdlsNull.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsNull.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::UnsupportedFormat);
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage.value().contains(QStringLiteral("empty")));
    }

    const QString mdlsNoPrefix = writeExecutableScript(
        dir,
        QStringLiteral("mdls_noprefix.sh"),
        QStringLiteral("#!/bin/sh\necho 'No useful metadata'\nexit 0\n"));
    QVERIFY(!mdlsNoPrefix.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsNoPrefix.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::UnsupportedFormat);
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage.value().contains(QStringLiteral("empty")));
    }

    const QString mdlsExitWithErr = writeExecutableScript(
        dir,
        QStringLiteral("mdls_fail_stderr.sh"),
        QStringLiteral("#!/bin/sh\necho 'boom' >&2\nexit 7\n"));
    QVERIFY(!mdlsExitWithErr.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsExitWithErr.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::UnsupportedFormat);
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage.value().contains(QStringLiteral("failed")));
    }

    const QString mdlsExitSilent = writeExecutableScript(
        dir,
        QStringLiteral("mdls_fail_silent.sh"),
        QStringLiteral("#!/bin/sh\nexit 2\n"));
    QVERIFY(!mdlsExitSilent.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsExitSilent.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::UnsupportedFormat);
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage.value().contains(QStringLiteral("Process failed")));
    }

    const QString mdlsSlow = writeExecutableScript(
        dir,
        QStringLiteral("mdls_slow.sh"),
        QStringLiteral("#!/bin/sh\nsleep 0.6\necho 'kMDItemTextContent = \"late\"'\nexit 0\n"));
    QVERIFY(!mdlsSlow.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportOk.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsSlow.toUtf8());
        ScopedEnvVar timeout("BS_TEST_MDLS_TIMEOUT_MS", "250");
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Timeout);
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage.value().contains(QStringLiteral("mdls timed out")));
    }

    const QString mdimportSlow = writeExecutableScript(
        dir,
        QStringLiteral("mdimport_slow.sh"),
        QStringLiteral("#!/bin/sh\nsleep 0.6\nexit 0\n"));
    QVERIFY(!mdimportSlow.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportSlow.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsQuoted.toUtf8());
        ScopedEnvVar timeout("BS_TEST_MDLS_TIMEOUT_MS", "250");
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Timeout);
        QVERIFY(result.errorMessage.has_value());
        QVERIFY(result.errorMessage.value().contains(QStringLiteral("mdimport timed out")));
    }

    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", "/path/does/not/exist/mdimport");
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsQuoted.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
        QVERIFY(result.content.has_value());
    }

    const QString mdimportFailWithErr = writeExecutableScript(
        dir,
        QStringLiteral("mdimport_fail_stderr.sh"),
        QStringLiteral("#!/bin/sh\necho 'mdimport unavailable' >&2\nexit 4\n"));
    QVERIFY(!mdimportFailWithErr.isEmpty());
    {
        ScopedEnvVar mdimportPath("BS_TEST_MDIMPORT_PATH", mdimportFailWithErr.toUtf8());
        ScopedEnvVar mdlsPath("BS_TEST_MDLS_PATH", mdlsQuoted.toUtf8());
        bs::MdlsTextExtractor extractor;
        const bs::ExtractionResult result = extractor.extract(filePath);
        QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
        QVERIFY(result.content.has_value());
    }
}

QTEST_MAIN(TestMdlsTextExtractor)
#include "test_mdls_text_extractor.moc"
