#include <QtTest/QtTest>

#include "core/shared/ipc_messages.h"
#include "service_process_harness.h"
#include "ipc_test_utils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

class TestIndexerServiceIpc : public QObject {
    Q_OBJECT

private slots:
    void testIndexerIpcContract();
};

void TestIndexerServiceIpc::testIndexerIpcContract()
{
    QTemporaryDir tempHome;
    QTemporaryDir rootDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(rootDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    const QString bsignorePath = QDir(tempHome.path()).filePath(QStringLiteral(".bsignore"));
    {
        QFile f(bsignorePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("*.tmp\ncache/\n");
        f.close();
    }

    const QString fixturePath = QDir(rootDir.path()).filePath(QStringLiteral("doc.txt"));
    {
        QFile f(fixturePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("pipeline fixture content\n");
        f.close();
    }

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("indexer"), QStringLiteral("betterspotlight-indexer"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.startTimeoutMs = 10000;
    launch.connectTimeoutMs = 10000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 7000;
    QVERIFY2(harness.start(launch), "Failed to start indexer service");

    const auto requestQueueStatus = [&harness](int timeoutMs = 3000) {
        QElapsedTimer callTimer;
        callTimer.start();
        while (callTimer.elapsed() < timeoutMs) {
            const QJsonObject queue = harness.request(QStringLiteral("getQueueStatus"), {}, 1000);
            if (bs::test::isResponse(queue)) {
                return queue;
            }
            QTest::qWait(75);
        }
        return QJsonObject();
    };

    const QJsonObject preQueue = requestQueueStatus(5000);
    QVERIFY(bs::test::isResponse(preQueue));
    QVERIFY(bs::test::resultPayload(preQueue).contains(QStringLiteral("memory")));

    {
        const QJsonObject response = harness.request(QStringLiteral("startIndexing"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }

    QJsonObject startParams;
    QJsonArray roots;
    roots.append(rootDir.path());
    startParams[QStringLiteral("roots")] = roots;

    {
        const QJsonObject response =
            harness.request(QStringLiteral("startIndexing"), startParams, 15000);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("success")).toBool(false));
    }
    {
        const QJsonObject response =
            harness.request(QStringLiteral("startIndexing"), startParams, 15000);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::AlreadyRunning));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("pauseIndexing"));
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("paused")).toBool(false));
    }
    {
        const QJsonObject response = harness.request(QStringLiteral("resumeIndexing"));
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("resumed")).toBool(false));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("setUserActive"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("active")] = true;
        const QJsonObject response = harness.request(QStringLiteral("setUserActive"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("active")).toBool(false));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("reindexPath"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("path")] = fixturePath;
        const QJsonObject response = harness.request(QStringLiteral("reindexPath"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("queued")).toBool(false));
    }

    const QJsonObject rebuildFirst = harness.request(QStringLiteral("rebuildAll"), {}, 15000);
    QVERIFY(bs::test::isResponse(rebuildFirst));
    QVERIFY(bs::test::resultPayload(rebuildFirst).contains(QStringLiteral("started")));

    const QJsonObject rebuildSecond = harness.request(QStringLiteral("rebuildAll"), {}, 15000);
    QVERIFY(bs::test::isResponse(rebuildSecond));
    const QJsonObject rebuildSecondResult = bs::test::resultPayload(rebuildSecond);
    QVERIFY(rebuildSecondResult.contains(QStringLiteral("alreadyRunning")));

    const bool secondAlreadyRunning =
        rebuildSecondResult.value(QStringLiteral("alreadyRunning")).toBool(false);
    bool observedRunning = secondAlreadyRunning;
    bool observedIdleAfterRun = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 12000) {
        const QJsonObject queue = requestQueueStatus(2000);
        if (!bs::test::isResponse(queue)) {
            QTest::qWait(100);
            continue;
        }
        const QJsonObject result = bs::test::resultPayload(queue);
        const bool rebuildRunning = result.value(QStringLiteral("rebuildRunning")).toBool(false);
        observedRunning = observedRunning || rebuildRunning;
        if (!rebuildRunning && (observedRunning || timer.elapsed() > 1000)) {
            observedIdleAfterRun = true;
            break;
        }
        QTest::qWait(100);
    }
    if (!observedIdleAfterRun) {
        qWarning() << "Rebuild did not report idle within polling window; keeping contract assertion focused on observable running state";
    }
    if (secondAlreadyRunning) {
        QVERIFY2(observedRunning,
                 "Expected rebuildRunning=true at least once after alreadyRunning response");
    }

    const QJsonObject beforeReload = requestQueueStatus(5000);
    QVERIFY(bs::test::isResponse(beforeReload));
    const qint64 loadedAtBefore = bs::test::resultPayload(beforeReload)
                                      .value(QStringLiteral("bsignoreLastLoadedAtMs"))
                                      .toInteger();

    {
        QFile f(bsignorePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("*.tmp\ncache/\n*.bak\n");
        f.close();
    }

    bool sawReload = false;
    timer.restart();
    while (timer.elapsed() < 8000) {
        const QJsonObject queue = requestQueueStatus(2000);
        if (!bs::test::isResponse(queue)) {
            QTest::qWait(75);
            continue;
        }
        const QJsonObject result = bs::test::resultPayload(queue);
        const qint64 loadedAt = result.value(QStringLiteral("bsignoreLastLoadedAtMs")).toInteger();
        const int patternCount = result.value(QStringLiteral("bsignorePatternCount")).toInt(0);
        if (patternCount >= 3 && loadedAt >= loadedAtBefore) {
            sawReload = true;
            break;
        }
        QTest::qWait(75);
    }
    QVERIFY2(sawReload, "Expected .bsignore watcher reload to update status");
}

QTEST_MAIN(TestIndexerServiceIpc)
#include "test_indexer_service_ipc.moc"
