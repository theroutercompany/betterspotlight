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
    void testServiceRuntimeJsonNormalizesLegacyStateOnlyRows();
    void testReadyRequiresRunningInvariant();
    void testServiceRuntimeJsonCanonicalizesLegacyFlagOnlyRows();
    void testServiceRuntimeJsonRejectsUnknownExplicitStates();
    void testManagedServiceRowsAreNormalizedBeforeHealthComputation();
    void testHealthComponentJsonNormalizesEmptyAndNegativeFields();
    void testOverallStatePrecedence();
    void testSnapshotEmitsV2Schema();
};

void TestHealthAggregatorActor::testServiceRuntimeJsonNormalizesLegacyStateOnlyRows()
{
    QJsonObject readyRow;
    readyRow[QStringLiteral("name")] = QStringLiteral(" query ");
    readyRow[QStringLiteral("state")] = QStringLiteral(" running ");
    readyRow[QStringLiteral("crashCount")] = -5;
    readyRow[QStringLiteral("pid")] = -99;
    readyRow[QStringLiteral("updatedAtMs")] = -1;
    readyRow[QStringLiteral("reason")] = QStringLiteral("  warmed up  ");

    const bs::ServiceRuntimeState parsed = bs::serviceRuntimeStateFromJson(readyRow);
    QCOMPARE(parsed.name, QStringLiteral("query"));
    QVERIFY(parsed.state == bs::ManagedServiceState::Ready);
    QVERIFY(parsed.running);
    QVERIFY(parsed.ready);
    QCOMPARE(parsed.crashCount, qint64{0});
    QCOMPARE(parsed.pid, qint64{0});
    QCOMPARE(parsed.updatedAtMs, qint64{0});
    QCOMPARE(parsed.reason, QStringLiteral("warmed up"));

    const QJsonObject encoded = bs::serviceRuntimeStateToJson(parsed);
    QCOMPARE(encoded.value(QStringLiteral("name")).toString(), QStringLiteral("query"));
    QCOMPARE(encoded.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    QVERIFY(encoded.value(QStringLiteral("running")).toBool());
    QVERIFY(encoded.value(QStringLiteral("ready")).toBool());
    QCOMPARE(encoded.value(QStringLiteral("crashCount")).toInteger(-1), qint64{0});
    QCOMPARE(encoded.value(QStringLiteral("pid")).toInteger(-1), qint64{0});
    QCOMPARE(encoded.value(QStringLiteral("updatedAtMs")).toInteger(-1), qint64{0});
    QCOMPARE(encoded.value(QStringLiteral("reason")).toString(), QStringLiteral("warmed up"));

    QJsonObject degradedRow;
    degradedRow[QStringLiteral("state")] = QStringLiteral("degraded");
    const bs::ServiceRuntimeState degraded = bs::serviceRuntimeStateFromJson(degradedRow);
    QVERIFY(degraded.running);
    QVERIFY(!degraded.ready);

    QJsonObject explicitStoppedRow;
    explicitStoppedRow[QStringLiteral("state")] = QStringLiteral("ready");
    explicitStoppedRow[QStringLiteral("running")] = false;
    explicitStoppedRow[QStringLiteral("ready")] = false;
    const bs::ServiceRuntimeState explicitStopped =
        bs::serviceRuntimeStateFromJson(explicitStoppedRow);
    QVERIFY(!explicitStopped.running);
    QVERIFY(!explicitStopped.ready);
}

void TestHealthAggregatorActor::testReadyRequiresRunningInvariant()
{
    QJsonObject legacyReadyOnly;
    legacyReadyOnly[QStringLiteral("name")] = QStringLiteral("query");
    legacyReadyOnly[QStringLiteral("ready")] = true;

    const bs::ServiceRuntimeState inferredRunning =
        bs::serviceRuntimeStateFromJson(legacyReadyOnly);
    QVERIFY(inferredRunning.state == bs::ManagedServiceState::Ready);
    QVERIFY(inferredRunning.running);
    QVERIFY(inferredRunning.ready);

    QJsonObject contradictoryRow;
    contradictoryRow[QStringLiteral("name")] = QStringLiteral("query");
    contradictoryRow[QStringLiteral("running")] = false;
    contradictoryRow[QStringLiteral("ready")] = true;

    const bs::ServiceRuntimeState stoppedWins =
        bs::serviceRuntimeStateFromJson(contradictoryRow);
    QVERIFY(stoppedWins.state == bs::ManagedServiceState::Stopped);
    QVERIFY(!stoppedWins.running);
    QVERIFY(!stoppedWins.ready);

    QJsonObject explicitStoppedReady;
    explicitStoppedReady[QStringLiteral("name")] = QStringLiteral("query");
    explicitStoppedReady[QStringLiteral("state")] = QStringLiteral("stopped");
    explicitStoppedReady[QStringLiteral("ready")] = true;

    const bs::ServiceRuntimeState stoppedStateWins =
        bs::serviceRuntimeStateFromJson(explicitStoppedReady);
    QVERIFY(stoppedStateWins.state == bs::ManagedServiceState::Stopped);
    QVERIFY(!stoppedStateWins.running);
    QVERIFY(!stoppedStateWins.ready);
}

void TestHealthAggregatorActor::testServiceRuntimeJsonCanonicalizesLegacyFlagOnlyRows()
{
    QJsonObject readyOnly;
    readyOnly[QStringLiteral("name")] = QStringLiteral("query");
    readyOnly[QStringLiteral("ready")] = true;

    const QJsonObject encodedReady =
        bs::serviceRuntimeStateToJson(bs::serviceRuntimeStateFromJson(readyOnly));
    QCOMPARE(encodedReady.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    QVERIFY(encodedReady.value(QStringLiteral("running")).toBool(false));
    QVERIFY(encodedReady.value(QStringLiteral("ready")).toBool(false));

    QJsonObject runningOnly;
    runningOnly[QStringLiteral("name")] = QStringLiteral("indexer");
    runningOnly[QStringLiteral("running")] = true;

    const QJsonObject encodedRunning =
        bs::serviceRuntimeStateToJson(bs::serviceRuntimeStateFromJson(runningOnly));
    QCOMPARE(encodedRunning.value(QStringLiteral("state")).toString(), QStringLiteral("starting"));
    QVERIFY(encodedRunning.value(QStringLiteral("running")).toBool(false));
    QVERIFY(!encodedRunning.value(QStringLiteral("ready")).toBool(true));
}

void TestHealthAggregatorActor::testServiceRuntimeJsonRejectsUnknownExplicitStates()
{
    QJsonObject invalidState;
    invalidState[QStringLiteral("name")] = QStringLiteral("query");
    invalidState[QStringLiteral("state")] = QStringLiteral("totally-fine");
    invalidState[QStringLiteral("running")] = true;
    invalidState[QStringLiteral("ready")] = true;

    const bs::ServiceRuntimeState parsed = bs::serviceRuntimeStateFromJson(invalidState);
    QVERIFY(parsed.state == bs::ManagedServiceState::Degraded);
    QVERIFY(!parsed.running);
    QVERIFY(!parsed.ready);
    QCOMPARE(parsed.reason, QStringLiteral("invalid_service_state"));

    bs::HealthAggregatorActor actor;
    actor.initialize(QStringLiteral("invalid-state-test"));
    actor.setManagedServices(QJsonArray{invalidState});
    QCOMPARE(actor.m_managedServices.size(), 1);

    const QJsonObject normalized = actor.m_managedServices.first().toObject();
    QCOMPARE(normalized.value(QStringLiteral("state")).toString(), QStringLiteral("degraded"));
    QVERIFY(!normalized.value(QStringLiteral("running")).toBool(true));
    QVERIFY(!normalized.value(QStringLiteral("ready")).toBool(true));

    QString reason;
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 QJsonArray{
                     invalidState,
                     makeService(QStringLiteral("indexer"), true, true, QStringLiteral("ready")),
                     makeService(QStringLiteral("extractor"), true, true, QStringLiteral("ready")),
                     makeService(QStringLiteral("inference"), true, true, QStringLiteral("ready")),
                 },
                 QJsonObject{},
                 0,
                 &reason),
             QStringLiteral("degraded"));
    QCOMPARE(reason, QStringLiteral("component_degraded"));
}

void TestHealthAggregatorActor::testManagedServiceRowsAreNormalizedBeforeHealthComputation()
{
    QJsonArray legacyRows;
    legacyRows.append(QStringLiteral("not an object"));
    legacyRows.append(QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")}});
    legacyRows.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral(" indexer ")},
        {QStringLiteral("state"), QStringLiteral(" running ")},
    });
    legacyRows.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("query")},
        {QStringLiteral("ready"), true},
    });
    legacyRows.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("inference")},
        {QStringLiteral("state"), QStringLiteral("ready")},
        {QStringLiteral("crashCount"), -9},
        {QStringLiteral("updatedAtMs"), -1},
    });
    legacyRows.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("extractor")},
        {QStringLiteral("state"), QStringLiteral("ready")},
    });

    QString reason;
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 legacyRows,
                 QJsonObject{},
                 0,
                 &reason),
             QStringLiteral("healthy"));
    QCOMPARE(reason, QStringLiteral("healthy"));

    bs::HealthAggregatorActor actor;
    actor.initialize(QStringLiteral("normalization-test"));
    actor.setManagedServices(legacyRows);
    QCOMPARE(actor.m_managedServices.size(), 4);

    const QJsonObject first = actor.m_managedServices.first().toObject();
    QCOMPARE(first.value(QStringLiteral("name")).toString(), QStringLiteral("indexer"));
    QCOMPARE(first.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    QVERIFY(first.value(QStringLiteral("running")).toBool(false));
    QVERIFY(first.value(QStringLiteral("ready")).toBool(false));

    const QJsonObject query = actor.m_managedServices.at(1).toObject();
    QCOMPARE(query.value(QStringLiteral("name")).toString(), QStringLiteral("query"));
    QCOMPARE(query.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    QVERIFY(query.value(QStringLiteral("running")).toBool(false));
    QVERIFY(query.value(QStringLiteral("ready")).toBool(false));

    const QJsonObject inference = actor.m_managedServices.at(2).toObject();
    QCOMPARE(inference.value(QStringLiteral("crashCount")).toInteger(-1), qint64{0});
    QCOMPARE(inference.value(QStringLiteral("updatedAtMs")).toInteger(-1), qint64{0});
}

void TestHealthAggregatorActor::testHealthComponentJsonNormalizesEmptyAndNegativeFields()
{
    bs::HealthComponentV2 component;
    component.state = QStringLiteral("   ");
    component.reason = QStringLiteral("  not ready  ");
    component.lastUpdatedMs = -20;
    component.stalenessMs = -7;
    component.metrics[QStringLiteral("preserved")] = true;

    const QJsonObject json = bs::healthComponentToJson(component);
    QCOMPARE(json.value(QStringLiteral("state")).toString(), QStringLiteral("unavailable"));
    QCOMPARE(json.value(QStringLiteral("reason")).toString(), QStringLiteral("not ready"));
    QCOMPARE(json.value(QStringLiteral("lastUpdatedMs")).toInteger(-1), qint64{0});
    QCOMPARE(json.value(QStringLiteral("stalenessMs")).toInteger(-1), qint64{0});
    QVERIFY(json.value(QStringLiteral("metrics"))
                .toObject()
                .value(QStringLiteral("preserved"))
                .toBool(false));
}

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

    QJsonArray missingRequiredServices = services;
    missingRequiredServices.removeAt(3);
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 missingRequiredServices,
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

    QJsonObject failedRebuildHealth;
    failedRebuildHealth[QStringLiteral("queueRebuildStatus")] = QStringLiteral("aborted");
    QCOMPARE(bs::HealthAggregatorActor::computeOverallState(
                 services,
                 failedRebuildHealth,
                 0,
                 &reason),
             QStringLiteral("degraded"));
    QCOMPARE(reason, QStringLiteral("component_degraded"));

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
