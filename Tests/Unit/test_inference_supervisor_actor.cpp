#include <QtTest/QtTest>

#include "services/inference/inference_supervisor_actor.h"

class TestInferenceSupervisorActor : public QObject {
    Q_OBJECT

private slots:
    void testThresholdAndRestartBudget();
    void testSuccessClearsGivingUpState();
    void testTimeoutRestoresAvailability();
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

QTEST_MAIN(TestInferenceSupervisorActor)
#include "test_inference_supervisor_actor.moc"
