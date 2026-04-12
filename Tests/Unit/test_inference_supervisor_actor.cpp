#include <QtTest/QtTest>

#include "services/inference/inference_worker_actor.h"
#include "services/inference/inference_supervisor_actor.h"

class TestInferenceSupervisorActor : public QObject {
    Q_OBJECT

private slots:
    void testThresholdAndRestartBudget();
    void testSuccessClearsGivingUpState();
    void testTimeoutRestoresAvailability();
    void testRebuildAdmissionDefersToLiveLanePressure();
};

void TestInferenceSupervisorActor::testThresholdAndRestartBudget()
{
    bs::InferenceSupervisorActor actor;
    const QString role = QStringLiteral("bi-encoder");

    auto d1 = actor.recordFailure(role);
    QVERIFY(!d1.restartRequested);

    auto d2 = actor.recordFailure(role);
    QVERIFY(!d2.restartRequested);

    auto d3 = actor.recordFailure(role);
    QVERIFY(d3.restartRequested);
    QVERIFY(d3.backoffMs > 0);
    QCOMPARE(d3.restartAttempts, 1);

    bs::InferenceSupervisorActor::RecoveryDecision decision;
    for (int i = 0; i < 16; ++i) {
        decision = actor.recordFailure(role);
        if (decision.givingUp) {
            break;
        }
    }

    QVERIFY(decision.givingUp);

    const QJsonObject exhausted = actor.restartBudgetExhaustedByRole();
    QVERIFY(exhausted.value(role).toBool(false));
}

void TestInferenceSupervisorActor::testSuccessClearsGivingUpState()
{
    bs::InferenceSupervisorActor actor;
    const QString role = QStringLiteral("cross-encoder");

    bs::InferenceSupervisorActor::RecoveryDecision decision;
    for (int i = 0; i < 20; ++i) {
        decision = actor.recordFailure(role);
        if (decision.givingUp) {
            break;
        }
    }
    QVERIFY(decision.givingUp);

    actor.recordSuccess(role);

    const QJsonObject states = actor.supervisorStateByRole();
    QCOMPARE(states.value(role).toString(), QStringLiteral("ready"));
    const QJsonObject exhausted = actor.restartBudgetExhaustedByRole();
    QVERIFY(!exhausted.value(role).toBool(true));
}

void TestInferenceSupervisorActor::testTimeoutRestoresAvailability()
{
    bs::InferenceSupervisorActor actor;
    const QString role = QStringLiteral("qa-extractive");

    actor.recordFailure(role);
    actor.recordFailure(role);
    actor.recordFailure(role);
    actor.markRoleUnavailable(role);

    actor.recordTimeout(role);

    const QJsonObject states = actor.supervisorStateByRole();
    QCOMPARE(states.value(role).toString(), QStringLiteral("ready"));
}

void TestInferenceSupervisorActor::testRebuildAdmissionDefersToLiveLanePressure()
{
    const auto decision = bs::InferenceWorkerActor::admitRebuild(
        /*workerRebuildDepth=*/0,
        /*workerRebuildLimit=*/2,
        /*globalRebuildDepth=*/0,
        /*globalRebuildLimit=*/4,
        /*workerLiveDepth=*/1,
        /*workerLiveLimit=*/2,
        /*globalLiveDepth=*/1,
        /*globalLiveLimit=*/4);

    QVERIFY(!decision.accepted);
    QCOMPARE(decision.reason, QStringLiteral("worker_live_lane_busy"));
    QCOMPARE(decision.priorityLaneName, QStringLiteral("live"));
    QCOMPARE(decision.priorityLaneDepth, 1);
    QCOMPARE(decision.priorityLaneLimit, 4);
    QCOMPARE(decision.laneQueueDepth, 0);
    QCOMPARE(decision.laneQueueLimit, 2);
    QCOMPARE(decision.globalLaneDepth, 0);
    QCOMPARE(decision.globalLaneLimit, 4);

    const QJsonObject json = bs::InferenceWorkerActor::toJson(decision);
    QCOMPARE(json.value(QStringLiteral("priorityLane")).toString(), QStringLiteral("live"));
    QCOMPARE(json.value(QStringLiteral("priorityLaneDepth")).toInt(), 1);
    QCOMPARE(json.value(QStringLiteral("priorityLaneLimit")).toInt(), 4);
}

QTEST_MAIN(TestInferenceSupervisorActor)
#include "test_inference_supervisor_actor.moc"
