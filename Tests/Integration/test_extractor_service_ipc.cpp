#include <QtTest/QtTest>

#include "core/shared/ipc_messages.h"
#include "ipc_test_utils.h"
#include "service_process_harness.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

class TestExtractorServiceIpc : public QObject {
    Q_OBJECT

private slots:
    void testExtractorIpcContract();
};

void TestExtractorServiceIpc::testExtractorIpcContract()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    const QString textPath = QDir(docsDir.path()).filePath(QStringLiteral("fixture.txt"));
    {
        QFile file(textPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("extractor service fixture text\n");
        file.close();
    }

    const QString execPath = QDir(docsDir.path()).filePath(QStringLiteral("run.sh"));
    {
        QFile file(execPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("#!/bin/sh\necho hello\n");
        file.close();
    }
    QVERIFY(QFile::setPermissions(execPath, QFileDevice::ReadUser | QFileDevice::WriteUser
                                             | QFileDevice::ExeUser));

    const QString symlinkPath = QDir(docsDir.path()).filePath(QStringLiteral("run-link.sh"));
    QFile::remove(symlinkPath);
    QVERIFY(QFile::link(execPath, symlinkPath));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("extractor"), QStringLiteral("betterspotlight-extractor"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.startTimeoutMs = 10000;
    launch.connectTimeoutMs = 10000;
    QVERIFY2(harness.start(launch), "Failed to start extractor service");

    {
        const QJsonObject response = harness.request(QStringLiteral("extractText"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("path")] = textPath;
        const QJsonObject response = harness.request(QStringLiteral("extractText"), params);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("path")] = textPath;
        params[QStringLiteral("kind")] = QStringLiteral("binary");
        const QJsonObject response = harness.request(QStringLiteral("extractText"), params);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::Unsupported));
    }
    {
        QJsonObject params;
        params[QStringLiteral("path")] = textPath;
        params[QStringLiteral("kind")] = QStringLiteral("text");
        const QJsonObject response = harness.request(QStringLiteral("extractText"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("text")).toString().contains(
            QStringLiteral("fixture"), Qt::CaseInsensitive));
        QVERIFY(result.contains(QStringLiteral("duration")));
    }

    {
        QJsonObject params;
        params[QStringLiteral("path")] = QStringLiteral("/no/such/file.txt");
        const QJsonObject response = harness.request(QStringLiteral("extractMetadata"), params);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::NotFound));
    }
    {
        QJsonObject params;
        params[QStringLiteral("path")] = execPath;
        const QJsonObject response = harness.request(QStringLiteral("extractMetadata"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("isExecutable")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("isSymlink")).toBool(true));
    }
    {
        QJsonObject params;
        params[QStringLiteral("path")] = symlinkPath;
        const QJsonObject response = harness.request(QStringLiteral("extractMetadata"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("isSymlink")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("symlinkTarget")).toString().isEmpty());
    }

    {
        QJsonObject params;
        params[QStringLiteral("extension")] = QStringLiteral(".TXT");
        const QJsonObject response = harness.request(QStringLiteral("isSupported"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("supported")).toBool(false));
    }
    {
        QJsonObject params;
        params[QStringLiteral("extension")] = QStringLiteral("Md");
        const QJsonObject response = harness.request(QStringLiteral("isSupported"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("supported")).toBool(false));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("cancelExtraction"));
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("cancelled")).toBool(false));
    }
    {
        const QJsonObject response = harness.request(QStringLiteral("clearExtractionCache"));
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).contains(QStringLiteral("removedCount")));
    }
    {
        const QJsonObject response = harness.request(QStringLiteral("clearCache"));
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).contains(QStringLiteral("removedCount")));
    }
}

QTEST_MAIN(TestExtractorServiceIpc)
#include "test_extractor_service_ipc.moc"

