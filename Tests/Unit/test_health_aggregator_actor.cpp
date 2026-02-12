#include <QtTest/QtTest>

#define private public
#include "app/control_plane/health_aggregator_actor.h"
#undef private

#include <QSignalSpy>

namespace {

QJsonObject makeService(const QString& name,
                        bool running,
                        bool ready,
                        const QString& state,
                        qint64 updatedAtMs = 0)
{
    QJsonObject row;
    row[QStringLiteral("name")] = name;
    row[QStringLiteral("running")] = running;
    row[QStringLiteral("ready")] = ready;
    row[QStringLiteral("state")] = state;
    row[QStringLiteral("updatedAtMs")] = updatedAtMs;
    return row;
}

QJsonArray readyServices()
{
    QJsonArray services;
    services.append(makeService(QStringLiteral("indexer"), true, true, QStringLiteral("ready")));
    services.append(makeService(QStringLiteral("extractor"), true, true, QStringLiteral("ready")));
    services.append(makeService(QStringLiteral("query"), true, true, QStringLiteral("ready")));
    services.append(makeService(QStringLiteral("inference"), true, true, QStringLiteral("ready")));
    return services;
}

} // namespace

class TestHealthAggregatorActor : public QObject {
    Q_OBJECT

private slots:
    void testOverallStatePrecedence();
    void testSnapshotEmitsV2Schema();
};

void TestHealthAggregatorActor::testOverallStatePrecedence()
{
    QString reason;
    const QJsonArray services = readyServices();

    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 services,
                 QJsonObject{},
                 7000,
                 &reason),
             QStringLiteral("stale"));
    QCOMPARE(reason, QStringLiteral("snapshot_stale"));

    QJsonArray unavailableServices = services;
    unavailableServices[0] = makeService(QStringLiteral("indexer"), false, false,
                                         QStringLiteral("stopped"));
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 unavailableServices,
                 QJsonObject{},
                 0,
                 &reason),
             QStringLiteral("unavailable"));
    QCOMPARE(reason, QStringLiteral("required_service_unavailable"));

    QJsonArray degradedServices = services;
    degradedServices[1] = makeService(QStringLiteral("extractor"), true, true,
                                      QStringLiteral("backoff"));
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 degradedServices,
                 QJsonObject{},
                 0,
                 &reason),
             QStringLiteral("degraded"));
    QCOMPARE(reason, QStringLiteral("component_degraded"));

    QJsonObject rebuildingHealth;
    rebuildingHealth[QStringLiteral("queueRebuildRunning")] = true;
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 services,
                 rebuildingHealth,
                 0,
                 &reason),
             QStringLiteral("rebuilding"));
    QCOMPARE(reason, QStringLiteral("rebuilding"));

    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 services,
                 QJsonObject{},
                 0,
                 &reason),
             QStringLiteral("healthy"));
    QCOMPARE(reason, QStringLiteral("healthy"));
}

void TestHealthAggregatorActor::testSnapshotEmitsV2Schema()
{
    bs::HealthAggregatorActor actor;
    actor.initialize(QStringLiteral("test-instance"));
    actor.setManagedServices(readyServices());

    QSignalSpy snapshotSpy(&actor, &bs::HealthAggregatorActor::snapshotUpdated);

    actor.start();
    actor.triggerRefresh();

    QTRY_VERIFY_WITH_TIMEOUT(snapshotSpy.count() > 0, 3000);

    const QJsonObject snapshot = qvariant_cast<QJsonObject>(snapshotSpy.takeLast().at(0));
    QCOMPARE(snapshot.value(QStringLiteral("schemaVersion")).toInt(), 2);
    QCOMPARE(snapshot.value(QStringLiteral("instanceId")).toString(),
             QStringLiteral("test-instance"));
    QVERIFY(snapshot.contains(QStringLiteral("snapshotId")));
    QVERIFY(snapshot.contains(QStringLiteral("snapshotTimeMs")));
    QVERIFY(snapshot.contains(QStringLiteral("stalenessMs")));
    QVERIFY(snapshot.contains(QStringLiteral("overall")));
    QVERIFY(snapshot.contains(QStringLiteral("components")));
    QVERIFY(snapshot.contains(QStringLiteral("queue")));
    QVERIFY(snapshot.contains(QStringLiteral("index")));
    QVERIFY(snapshot.contains(QStringLiteral("vector")));
    QVERIFY(snapshot.contains(QStringLiteral("inference")));
    QVERIFY(snapshot.contains(QStringLiteral("processes")));
    QVERIFY(snapshot.contains(QStringLiteral("errors")));

    actor.stop();
}

QTEST_MAIN(TestHealthAggregatorActor)
#include "test_health_aggregator_actor.moc"
