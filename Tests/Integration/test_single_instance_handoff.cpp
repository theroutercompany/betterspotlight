#include <QtTest/QtTest>

#include <QLockFile>
#include <QTemporaryDir>

class TestSingleInstanceHandoff : public QObject {
    Q_OBJECT

private slots:
    void testSecondLockIsRejectedUntilPrimaryReleases();
};

void TestSingleInstanceHandoff::testSecondLockIsRejectedUntilPrimaryReleases()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString lockPath = tempDir.filePath(QStringLiteral("app.lock"));

    QLockFile primary(lockPath);
    QLockFile secondary(lockPath);
    primary.setStaleLockTime(0);
    secondary.setStaleLockTime(0);

    QVERIFY(primary.tryLock(0));
    QVERIFY(!secondary.tryLock(0));

    qint64 ownerPid = 0;
    QString ownerHost;
    QString ownerApp;
    QVERIFY(secondary.getLockInfo(&ownerPid, &ownerHost, &ownerApp));
    QVERIFY(ownerPid > 0);

    primary.unlock();
    QVERIFY(secondary.tryLock(0));
    secondary.unlock();
}

QTEST_MAIN(TestSingleInstanceHandoff)
#include "test_single_instance_handoff.moc"
