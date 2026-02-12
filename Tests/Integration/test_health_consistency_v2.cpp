#include <QtTest/QtTest>

#include "app/search_controller.h"
#include "app/service_manager.h"
#include "app/control_plane/health_snapshot_v2.h"

class TestHealthConsistencyV2 : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testSearchControllerUsesActorSnapshotInAggregatorPrimary();
    void testSnapshotSchemaContainsRequiredSections();
};

void TestHealthConsistencyV2::initTestCase()
{
    qputenv("BETTERSPOTLIGHT_HEALTH_SOURCE_MODE", QByteArray("aggregator_primary"));
}

void TestHealthConsistencyV2::testSearchControllerUsesActorSnapshotInAggregatorPrimary()
{
    bs::ServiceManager manager;
    bs::SearchController controller;
    controller.setServiceManager(&manager);

    QJsonObject snapshot;
    snapshot[QStringLiteral("schemaVersion")] = 2;
    snapshot[QStringLiteral("snapshotId")] = QStringLiteral("inst:1");
    snapshot[QStringLiteral("snapshotTimeMs")] = static_cast<qint64>(1);
    snapshot[QStringLiteral("stalenessMs")] = static_cast<qint64>(0);
    snapshot[QStringLiteral("instanceId")] = QStringLiteral("inst");
    snapshot[QStringLiteral("overallStatus")] = QStringLiteral("degraded");
    snapshot[QStringLiteral("snapshotState")] = QStringLiteral("fresh");
    snapshot[QStringLiteral("healthStatusReason")] = QStringLiteral("component_degraded");

    QJsonObject processes;
    QJsonArray managed;
    QJsonObject row;
    row[QStringLiteral("name")] = QStringLiteral("indexer");
    row[QStringLiteral("running")] = true;
    row[QStringLiteral("ready")] = true;
    row[QStringLiteral("state")] = QStringLiteral("ready");
    managed.append(row);
    processes[QStringLiteral("managed")] = managed;
    snapshot[QStringLiteral("processes")] = processes;

    QVERIFY(QMetaObject::invokeMethod(
        &manager,
        "onHealthSnapshotUpdated",
        Qt::DirectConnection,
        Q_ARG(QJsonObject, snapshot)));

    const QVariantMap health = controller.getHealthSync();
    QCOMPARE(health.value(QStringLiteral("overallStatus")).toString(),
             QStringLiteral("degraded"));
    QCOMPARE(health.value(QStringLiteral("snapshotState")).toString(),
             QStringLiteral("fresh"));

    const QVariantMap processMap = health.value(QStringLiteral("processes")).toMap();
    QVERIFY(!processMap.value(QStringLiteral("managed")).toList().isEmpty());
}

void TestHealthConsistencyV2::testSnapshotSchemaContainsRequiredSections()
{
    bs::HealthSnapshotV2 snapshot;
    snapshot.instanceId = QStringLiteral("instance");
    snapshot.snapshotTimeMs = 123;
    snapshot.snapshotId = QStringLiteral("instance:123");
    snapshot.stalenessMs = 5;
    snapshot.overallState = QStringLiteral("healthy");
    snapshot.overallReason = QStringLiteral("healthy");
    snapshot.components = QJsonObject{{QStringLiteral("query"), QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")}}}};
    snapshot.queue = QJsonObject{{QStringLiteral("pending"), 0}};
    snapshot.index = QJsonObject{{QStringLiteral("files"), 10}};
    snapshot.vector = QJsonObject{{QStringLiteral("activeEmbedded"), 10}};
    snapshot.inference = QJsonObject{{QStringLiteral("connected"), true}};
    snapshot.processes = QJsonObject{{QStringLiteral("managed"), QJsonArray{}}};
    snapshot.errors = QJsonArray{};

    const QJsonObject json = bs::toJson(snapshot);
    QCOMPARE(json.value(QStringLiteral("schemaVersion")).toInt(), 2);
    QVERIFY(json.contains(QStringLiteral("overall")));
    QVERIFY(json.contains(QStringLiteral("components")));
    QVERIFY(json.contains(QStringLiteral("queue")));
    QVERIFY(json.contains(QStringLiteral("index")));
    QVERIFY(json.contains(QStringLiteral("vector")));
    QVERIFY(json.contains(QStringLiteral("inference")));
    QVERIFY(json.contains(QStringLiteral("processes")));
    QVERIFY(json.contains(QStringLiteral("errors")));
}

QTEST_MAIN(TestHealthConsistencyV2)
#include "test_health_consistency_v2.moc"
