#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#define private public
#include "core/ipc/supervisor.h"
#undef private

#include "core/ipc/message.h"
#include "core/ipc/service_base.h"
#include "core/ipc/socket_server.h"

namespace {

QString uniqueServiceName(const QString& prefix)
{
    return prefix + QStringLiteral("-%1-%2")
                        .arg(QCoreApplication::applicationPid())
                        .arg(QDateTime::currentMSecsSinceEpoch());
}

void removeSocketPath(const QString& socketPath)
{
    QFile::remove(socketPath);
}

bool writeExecutableScript(const QString& path, const QByteArray& contents)
{
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (script.write(contents) != contents.size()) {
        return false;
    }
    script.close();
    return QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            | QFileDevice::ReadGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

} // namespace

class TestSupervisor : public QObject {
    Q_OBJECT

private slots:
    void testAddServiceIsIdempotentAndUpdatesPath();
    void testRuntimeDirectoriesRespectEnvironment();
    void testStartAllReportsFailureForMissingBinary();
    void testHeartbeatTransitionsOnPingAndErrorResponses();
    void testStopAllEscalatesToKillForStubbornProcess();
    void testCrashThresholdAndWindowResetPath();
    void testNormalExitEmitsServiceStopped();
};

void TestSupervisor::testAddServiceIsIdempotentAndUpdatesPath()
{
    bs::Supervisor supervisor;
    const QString serviceName = uniqueServiceName(QStringLiteral("dup"));

    supervisor.addService(serviceName, QStringLiteral("/bin/cat"));
    supervisor.addService(serviceName, QStringLiteral("/bin/echo"));

    auto* svc = supervisor.findService(serviceName);
    QVERIFY(svc != nullptr);
    QCOMPARE(svc->info.executablePath, QStringLiteral("/bin/echo"));

    const QJsonArray snapshot = supervisor.serviceSnapshot();
    QCOMPARE(snapshot.size(), 1);
    const QJsonObject row = snapshot.first().toObject();
    QCOMPARE(row.value(QStringLiteral("name")).toString(), serviceName);
    QCOMPARE(row.value(QStringLiteral("state")).toString(), QStringLiteral("registered"));
}

void TestSupervisor::testRuntimeDirectoriesRespectEnvironment()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const bool hadSocketEnv = qEnvironmentVariableIsSet("BETTERSPOTLIGHT_SOCKET_DIR");
    const bool hadPidEnv = qEnvironmentVariableIsSet("BETTERSPOTLIGHT_PID_DIR");
    const QByteArray prevSocketEnv = qgetenv("BETTERSPOTLIGHT_SOCKET_DIR");
    const QByteArray prevPidEnv = qgetenv("BETTERSPOTLIGHT_PID_DIR");

    const QString socketDir = tempDir.path() + QStringLiteral("/runtime/sockets");
    const QString pidDir = tempDir.path() + QStringLiteral("/runtime/pids");
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.toUtf8());
    qputenv("BETTERSPOTLIGHT_PID_DIR", pidDir.toUtf8());

    bs::Supervisor supervisor;
    supervisor.addService(uniqueServiceName(QStringLiteral("missing-dir-check")),
                          QStringLiteral("/definitely/not/a/real/binary"));
    QVERIFY(!supervisor.startAll());

    QVERIFY(QDir(socketDir).exists());
    QVERIFY(QDir(pidDir).exists());

    supervisor.stopAll();
    if (hadSocketEnv) {
        qputenv("BETTERSPOTLIGHT_SOCKET_DIR", prevSocketEnv);
    } else {
        qunsetenv("BETTERSPOTLIGHT_SOCKET_DIR");
    }
    if (hadPidEnv) {
        qputenv("BETTERSPOTLIGHT_PID_DIR", prevPidEnv);
    } else {
        qunsetenv("BETTERSPOTLIGHT_PID_DIR");
    }
}

void TestSupervisor::testStartAllReportsFailureForMissingBinary()
{
    bs::Supervisor supervisor;
    const QString serviceName = uniqueServiceName(QStringLiteral("missing"));
    supervisor.addService(serviceName, QStringLiteral("/definitely/not/a/real/binary"));

    QVERIFY(!supervisor.startAll());

    const QJsonArray snapshot = supervisor.serviceSnapshot();
    QCOMPARE(snapshot.size(), 1);
    const QJsonObject svc = snapshot.first().toObject();
    QCOMPARE(svc.value(QStringLiteral("name")).toString(), serviceName);
    QVERIFY(!svc.value(QStringLiteral("running")).toBool());
    QCOMPARE(svc.value(QStringLiteral("pid")).toInteger(0), 0);

    supervisor.stopAll();
}

void TestSupervisor::testHeartbeatTransitionsOnPingAndErrorResponses()
{
    const QString serviceName = uniqueServiceName(QStringLiteral("heartbeat"));
    const QString socketPath = bs::ServiceBase::socketPath(serviceName);
    removeSocketPath(socketPath);
    QVERIFY(QDir().mkpath(QFileInfo(socketPath).absolutePath()));

    enum class PingMode {
        Ok,
        Error,
    };
    PingMode mode = PingMode::Ok;

    bs::SocketServer mockServer;
    mockServer.setRequestHandler([&mode](const QJsonObject& request) {
        const uint64_t id = static_cast<uint64_t>(
            request.value(QStringLiteral("id")).toInteger());
        const QString method = request.value(QStringLiteral("method")).toString();

        if (method == QLatin1String("ping")) {
            if (mode == PingMode::Error) {
                return bs::IpcMessage::makeError(id,
                                                 bs::IpcErrorCode::InternalError,
                                                 QStringLiteral("forced heartbeat failure"));
            }
            QJsonObject result;
            result[QStringLiteral("pong")] = true;
            return bs::IpcMessage::makeResponse(id, result);
        }
        if (method == QLatin1String("shutdown")) {
            QJsonObject result;
            result[QStringLiteral("shutting_down")] = true;
            return bs::IpcMessage::makeResponse(id, result);
        }
        return bs::IpcMessage::makeError(id,
                                         bs::IpcErrorCode::NotFound,
                                         QStringLiteral("unsupported method"));
    });
    QVERIFY(mockServer.listen(socketPath));

    bs::Supervisor supervisor;
    QSignalSpy startedSpy(&supervisor, &bs::Supervisor::serviceStarted);
    QSignalSpy allReadySpy(&supervisor, &bs::Supervisor::allServicesReady);

    supervisor.addService(serviceName, QStringLiteral("/bin/cat"));
    QVERIFY(supervisor.startAll());

    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() >= 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(allReadySpy.count() >= 1, 8000);

    QVERIFY(supervisor.clientFor(serviceName) != nullptr);
    QVERIFY(supervisor.clientFor(serviceName + QStringLiteral("-missing")) == nullptr);

    QJsonArray snapshot = supervisor.serviceSnapshot();
    QCOMPARE(snapshot.size(), 1);
    QVERIFY(snapshot.first().toObject().value(QStringLiteral("running")).toBool());
    QVERIFY(snapshot.first().toObject().value(QStringLiteral("ready")).toBool());
    QVERIFY(snapshot.first().toObject().value(QStringLiteral("pid")).toInteger(0) > 0);

    mode = PingMode::Error;
    supervisor.heartbeat();
    snapshot = supervisor.serviceSnapshot();
    QVERIFY(!snapshot.first().toObject().value(QStringLiteral("ready")).toBool());

    mode = PingMode::Ok;
    supervisor.heartbeat();
    snapshot = supervisor.serviceSnapshot();
    QVERIFY(snapshot.first().toObject().value(QStringLiteral("ready")).toBool());

    // Close stdin so /bin/cat exits quickly and stopAll remains fast.
    auto* svc = supervisor.findService(serviceName);
    QVERIFY(svc != nullptr);
    QVERIFY(svc->process != nullptr);
    svc->process->closeWriteChannel();
    QTRY_VERIFY_WITH_TIMEOUT(svc->process->state() == QProcess::NotRunning, 3000);

    supervisor.stopAll();
    mockServer.close();
    removeSocketPath(socketPath);
}

void TestSupervisor::testStopAllEscalatesToKillForStubbornProcess()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString scriptPath = tempDir.path() + QStringLiteral("/ignore-term.sh");
    const QByteArray script =
        "#!/bin/sh\n"
        "trap '' TERM\n"
        "while true; do\n"
        "  sleep 1\n"
        "done\n";
    QVERIFY(writeExecutableScript(scriptPath, script));

    const QString serviceName = uniqueServiceName(QStringLiteral("stubborn"));
    const QString socketPath = bs::ServiceBase::socketPath(serviceName);
    removeSocketPath(socketPath);
    QVERIFY(QDir().mkpath(QFileInfo(socketPath).absolutePath()));

    bs::SocketServer mockServer;
    mockServer.setRequestHandler([](const QJsonObject& request) {
        const uint64_t id = static_cast<uint64_t>(
            request.value(QStringLiteral("id")).toInteger());
        const QString method = request.value(QStringLiteral("method")).toString();
        if (method == QLatin1String("shutdown")
            || method == QLatin1String("ping")) {
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            return bs::IpcMessage::makeResponse(id, result);
        }
        return bs::IpcMessage::makeError(id,
                                         bs::IpcErrorCode::NotFound,
                                         QStringLiteral("unsupported method"));
    });
    QVERIFY(mockServer.listen(socketPath));

    bs::Supervisor supervisor;
    QSignalSpy stopSpy(&supervisor, &bs::Supervisor::serviceStopped);

    supervisor.addService(serviceName, scriptPath);
    QVERIFY(supervisor.startAll());

    auto connectedPredicate = [&supervisor, &serviceName]() {
        auto* svc = supervisor.findService(serviceName);
        return svc && svc->process && svc->process->state() == QProcess::Running
               && svc->client && svc->client->isConnected();
    };
    QTRY_VERIFY_WITH_TIMEOUT(connectedPredicate(), 8000);

    QElapsedTimer timer;
    timer.start();
    supervisor.stopAll();
    QVERIFY2(timer.elapsed() >= 4500,
             "Expected supervisor to wait for graceful exit before escalating");

    auto* svc = supervisor.findService(serviceName);
    QVERIFY(svc != nullptr);
    if (svc->process) {
        QVERIFY(svc->process->state() == QProcess::NotRunning);
    }
    QVERIFY(stopSpy.count() >= 1);

    mockServer.close();
    removeSocketPath(socketPath);
}

void TestSupervisor::testCrashThresholdAndWindowResetPath()
{
    bs::Supervisor supervisor;
    const QString serviceName = uniqueServiceName(QStringLiteral("crasher"));

    QSignalSpy crashSpy(&supervisor, &bs::Supervisor::serviceCrashed);
    supervisor.addService(serviceName, QStringLiteral("/usr/bin/false"));
    QVERIFY(supervisor.startAll());

    QTRY_VERIFY_WITH_TIMEOUT(crashSpy.count() >= bs::Supervisor::kMaxCrashesBeforeGiveUp,
                             15000);
    const int crashCountAtThreshold = crashSpy.count();
    QTest::qWait(2500);
    QCOMPARE(crashSpy.count(), crashCountAtThreshold);

    auto* svc = supervisor.findService(serviceName);
    QVERIFY(svc != nullptr);
    svc->info.executablePath = QStringLiteral("/definitely/not/a/real/binary");
    svc->info.crashCount = bs::Supervisor::kMaxCrashesBeforeGiveUp;
    svc->info.firstCrashTime = QDateTime::currentSecsSinceEpoch() - 120;
    svc->info.lastCrashTime = QDateTime::currentSecsSinceEpoch()
        - (bs::Supervisor::kCrashWindowSeconds * 2 + 5);

    supervisor.heartbeat();
    QCOMPARE(svc->info.crashCount, 0);
    QCOMPARE(svc->info.firstCrashTime, static_cast<int64_t>(0));

    supervisor.stopAll();
}

void TestSupervisor::testNormalExitEmitsServiceStopped()
{
    bs::Supervisor supervisor;
    const QString serviceName = uniqueServiceName(QStringLiteral("normal"));

    QSignalSpy stopSpy(&supervisor, &bs::Supervisor::serviceStopped);
    supervisor.addService(serviceName, QStringLiteral("/usr/bin/true"));
    QVERIFY(supervisor.startAll());

    QTRY_VERIFY_WITH_TIMEOUT(stopSpy.count() >= 1, 5000);
    bool sawNamedStop = false;
    for (int i = 0; i < stopSpy.count(); ++i) {
        if (stopSpy.at(i).at(0).toString() == serviceName) {
            sawNamedStop = true;
            break;
        }
    }
    QVERIFY(sawNamedStop);

    supervisor.stopAll();
}

QTEST_MAIN(TestSupervisor)
#include "test_supervisor.moc"
