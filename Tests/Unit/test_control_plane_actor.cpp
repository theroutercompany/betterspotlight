#include <QtTest/QtTest>

#include "app/control_plane/control_plane_actor.h"

#include <QSignalSpy>
#include <QVariantMap>

class TestControlPlaneActor : public QObject {
    Q_OBJECT

private slots:
    void testLifecycleTransitionsAndIdempotentStop();
    void testSuppressesStatusSignalsAfterShutdownPhase();
    void testConfigureServicesUsesIdempotentRegistration();
};

void TestControlPlaneActor::testLifecycleTransitionsAndIdempotentStop()
{
    bs::ControlPlaneActor actor;
    actor.initialize();

    QSignalSpy lifecycleSpy(&actor, &bs::ControlPlaneActor::lifecyclePhaseChanged);

    actor.setLifecyclePhase(QStringLiteral("running"));
    QCOMPARE(actor.lifecyclePhase(), QStringLiteral("running"));

    actor.stopAll();
    actor.stopAll();

    QCOMPARE(actor.lifecyclePhase(), QStringLiteral("stopped"));
    QVERIFY(lifecycleSpy.count() >= 3);
}

void TestControlPlaneActor::testSuppressesStatusSignalsAfterShutdownPhase()
{
    bs::ControlPlaneActor actor;
    actor.initialize();

    QSignalSpy statusSpy(&actor, &bs::ControlPlaneActor::serviceStatusChanged);

    actor.setLifecyclePhase(QStringLiteral("running"));
    QVERIFY(QMetaObject::invokeMethod(
        &actor,
        "onSupervisorServiceStarted",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("indexer"))));
    QCOMPARE(statusSpy.count(), 1);

    actor.setLifecyclePhase(QStringLiteral("shutting_down"));
    QVERIFY(QMetaObject::invokeMethod(
        &actor,
        "onSupervisorServiceStarted",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("query"))));
    QCOMPARE(statusSpy.count(), 1);
}

void TestControlPlaneActor::testConfigureServicesUsesIdempotentRegistration()
{
    bs::ControlPlaneActor actor;
    actor.initialize();

    QVariantList descriptors;

    QVariantMap first;
    first[QStringLiteral("name")] = QStringLiteral("indexer");
    first[QStringLiteral("binary")] = QStringLiteral("/bin/cat");
    descriptors.push_back(first);

    QVariantMap second;
    second[QStringLiteral("name")] = QStringLiteral("indexer");
    second[QStringLiteral("binary")] = QStringLiteral("/bin/echo");
    descriptors.push_back(second);

    actor.configureServices(descriptors);

    const QJsonArray snapshot = actor.serviceSnapshotSync();
    QCOMPARE(snapshot.size(), 1);
    QCOMPARE(snapshot.first().toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("indexer"));
}

QTEST_MAIN(TestControlPlaneActor)
#include "test_control_plane_actor.moc"
