#include <QtTest/QtTest>

#include "app/runtime_environment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

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
    void testCleanupRemovesOnlyStaleRuntimeDirectories();
};

void TestOrphanReconciliation::testCleanupRemovesOnlyStaleRuntimeDirectories()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString activeDir = tempDir.filePath(QStringLiteral("active-instance"));
    const QString liveDir = tempDir.filePath(QStringLiteral("live-instance"));
    const QString staleDir = tempDir.filePath(QStringLiteral("stale-instance"));

    QVERIFY(QDir().mkpath(activeDir));
    QVERIFY(writeInstanceMetadata(liveDir, static_cast<qint64>(QCoreApplication::applicationPid())));
    QVERIFY(writeInstanceMetadata(staleDir, static_cast<qint64>(999999)));

    bs::RuntimeContext context;
    context.runtimeRoot = tempDir.path();
    context.runtimeDir = activeDir;

    QStringList removed;
    bs::cleanupOrphanRuntimeDirectories(context, &removed);

    QVERIFY(QDir(activeDir).exists());
    QVERIFY(QDir(liveDir).exists());
    QVERIFY(!QDir(staleDir).exists());
    QVERIFY(removed.contains(staleDir));
}

QTEST_MAIN(TestOrphanReconciliation)
#include "test_orphan_reconciliation.moc"
