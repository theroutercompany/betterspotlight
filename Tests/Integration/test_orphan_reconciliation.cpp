#include <QtTest/QtTest>

#include "app/runtime_environment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

class EnvVarGuard {
public:
    explicit EnvVarGuard(const char* variableName)
        : name(variableName),
          hadValue(qEnvironmentVariableIsSet(variableName)),
          previousValue(qgetenv(variableName))
    {
    }

    ~EnvVarGuard()
    {
        if (hadValue) {
            qputenv(name.constData(), previousValue);
        } else {
            qunsetenv(name.constData());
        }
    }

private:
    QByteArray name;
    bool hadValue = false;
    QByteArray previousValue;
};

bool writeInstanceMetadata(const QString& dir, qint64 pid)
{
    if (!QDir().mkpath(dir)) {
        return false;
    }

    QFile file(QDir(dir).filePath(QStringLiteral("instance.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    QJsonObject json;
    json[QStringLiteral("instance_id")] = QFileInfo(dir).fileName();
    json[QStringLiteral("app_pid")] = pid;
    json[QStringLiteral("runtime_dir")] = dir;
    file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

} // namespace

class TestOrphanReconciliation : public QObject {
    Q_OBJECT

private slots:
    void testProcessIsAliveHandlesInvalidAndCurrentPid();
    void testInitRuntimeContextRejectsNullOutput();
    void testInitRuntimeContextHonorsEnvironmentOverridesAndWritesMetadata();
    void testCleanupRemovesOnlyStaleRuntimeDirectories();
};

void TestOrphanReconciliation::testProcessIsAliveHandlesInvalidAndCurrentPid()
{
    QVERIFY(!bs::processIsAlive(-1));
    QVERIFY(!bs::processIsAlive(0));
    QVERIFY(bs::processIsAlive(static_cast<qint64>(QCoreApplication::applicationPid())));
}

void TestOrphanReconciliation::testInitRuntimeContextRejectsNullOutput()
{
    QString error;
    QVERIFY(!bs::initRuntimeContext(nullptr, &error));
    QCOMPARE(error, QStringLiteral("Runtime context output is null"));
}

void TestOrphanReconciliation::testInitRuntimeContextHonorsEnvironmentOverridesAndWritesMetadata()
{
    EnvVarGuard runtimeGuard("BETTERSPOTLIGHT_RUNTIME_DIR");
    EnvVarGuard socketGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    EnvVarGuard pidGuard("BETTERSPOTLIGHT_PID_DIR");
    EnvVarGuard instanceGuard("BETTERSPOTLIGHT_INSTANCE_ID");

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString runtimeDir = QDir::cleanPath(tempDir.filePath(QStringLiteral("runtime")));
    const QString socketDir = QDir::cleanPath(tempDir.filePath(QStringLiteral("runtime/sockets")));
    const QString pidDir = QDir::cleanPath(tempDir.filePath(QStringLiteral("runtime/pids")));
    qputenv("BETTERSPOTLIGHT_RUNTIME_DIR", runtimeDir.toUtf8());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.toUtf8());
    qputenv("BETTERSPOTLIGHT_PID_DIR", pidDir.toUtf8());
    qputenv("BETTERSPOTLIGHT_INSTANCE_ID", QByteArray("test-instance"));

    bs::RuntimeContext context;
    QString error;
    QVERIFY2(bs::initRuntimeContext(&context, &error), qPrintable(error));

    QCOMPARE(context.instanceId, QStringLiteral("test-instance"));
    QCOMPARE(context.runtimeDir, runtimeDir);
    QCOMPARE(context.socketDir, socketDir);
    QCOMPARE(context.pidDir, pidDir);
    QCOMPARE(context.metadataPath, QDir(runtimeDir).filePath(QStringLiteral("instance.json")));
    QCOMPARE(context.lockPath,
             QDir(bs::runtimeRootPath()).filePath(QStringLiteral("app.lock")));

    QVERIFY(QDir(context.runtimeDir).exists());
    QVERIFY(QDir(context.socketDir).exists());
    QVERIFY(QDir(context.pidDir).exists());

    QCOMPARE(qEnvironmentVariable("BETTERSPOTLIGHT_RUNTIME_DIR"), context.runtimeDir);
    QCOMPARE(qEnvironmentVariable("BETTERSPOTLIGHT_SOCKET_DIR"), context.socketDir);
    QCOMPARE(qEnvironmentVariable("BETTERSPOTLIGHT_PID_DIR"), context.pidDir);
    QCOMPARE(qEnvironmentVariable("BETTERSPOTLIGHT_INSTANCE_ID"), context.instanceId);

    QFile metadataFile(context.metadataPath);
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const QJsonDocument metadataDoc = QJsonDocument::fromJson(metadataFile.readAll());
    QVERIFY(metadataDoc.isObject());
    const QJsonObject metadata = metadataDoc.object();
    QCOMPARE(metadata.value(QStringLiteral("instance_id")).toString(), context.instanceId);
    QCOMPARE(metadata.value(QStringLiteral("app_pid")).toInteger(),
             static_cast<qint64>(QCoreApplication::applicationPid()));
    QCOMPARE(metadata.value(QStringLiteral("runtime_dir")).toString(), context.runtimeDir);
    QCOMPARE(metadata.value(QStringLiteral("socket_dir")).toString(), context.socketDir);
    QCOMPARE(metadata.value(QStringLiteral("pid_dir")).toString(), context.pidDir);
    QVERIFY(!metadata.value(QStringLiteral("started_at")).toString().isEmpty());
}

void TestOrphanReconciliation::testCleanupRemovesOnlyStaleRuntimeDirectories()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString activeDir = tempDir.filePath(QStringLiteral("active-instance"));
    const QString liveDir = tempDir.filePath(QStringLiteral("live-instance"));
    const QString staleDir = tempDir.filePath(QStringLiteral("stale-instance"));
    const QString missingMetadataDir = tempDir.filePath(QStringLiteral("missing-metadata"));
    const QString invalidMetadataDir = tempDir.filePath(QStringLiteral("invalid-metadata"));

    QVERIFY(QDir().mkpath(activeDir));
    QVERIFY(writeInstanceMetadata(liveDir, static_cast<qint64>(QCoreApplication::applicationPid())));
    QVERIFY(writeInstanceMetadata(staleDir, static_cast<qint64>(999999)));
    QVERIFY(QDir().mkpath(missingMetadataDir));
    QVERIFY(QDir().mkpath(invalidMetadataDir));
    QFile invalidMetadata(QDir(invalidMetadataDir).filePath(QStringLiteral("instance.json")));
    QVERIFY(invalidMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(invalidMetadata.write("not-json") > 0);
    invalidMetadata.close();

    bs::RuntimeContext context;
    context.runtimeRoot = tempDir.path();
    context.runtimeDir = activeDir;

    QStringList removed;
    bs::cleanupOrphanRuntimeDirectories(context, &removed);

    QVERIFY(QDir(activeDir).exists());
    QVERIFY(QDir(liveDir).exists());
    QVERIFY(QDir(missingMetadataDir).exists());
    QVERIFY(QDir(invalidMetadataDir).exists());
    QVERIFY(!QDir(staleDir).exists());
    QVERIFY(removed.contains(staleDir));
    QVERIFY(!removed.contains(missingMetadataDir));
    QVERIFY(!removed.contains(invalidMetadataDir));
}

QTEST_MAIN(TestOrphanReconciliation)
#include "test_orphan_reconciliation.moc"
