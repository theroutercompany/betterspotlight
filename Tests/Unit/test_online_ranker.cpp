#include <QtTest/QtTest>

#include "core/learning/online_ranker.h"

#include <limits>

class TestOnlineRanker : public QObject {
    Q_OBJECT

private slots:
    void testRejectsInvalidCandidateEval();
    void testRejectsLatencyBudgetExceeded();
    void testRejectsLatencyRegressionExceeded();
    void testRejectsFailureRateExceeded();
    void testRejectsSaturationRateExceeded();
    void testAcceptsHealthyCandidateMetrics();
};

void TestOnlineRanker::testRejectsInvalidCandidateEval()
{
    bs::OnlineRanker::TrainConfig cfg;
    bs::OnlineRanker::TrainMetrics active;
    active.examples = 20;
    active.logLoss = 0.69;
    active.avgPredictionLatencyUs = 120.0;
    active.predictionFailureRate = 0.0;
    active.probabilitySaturationRate = 0.0;

    bs::OnlineRanker::TrainMetrics candidate;
    candidate.examples = 20;
    candidate.logLoss = std::numeric_limits<double>::quiet_NaN();
    candidate.avgPredictionLatencyUs = 90.0;
    candidate.predictionFailureRate = 0.0;
    candidate.probabilitySaturationRate = 0.0;

    QString reason;
    const bool allowed = bs::OnlineRanker::passesPromotionRuntimeGates(cfg, active, candidate, &reason);
    QVERIFY(!allowed);
    QCOMPARE(reason, QStringLiteral("candidate_stability_invalid_eval"));
}

void TestOnlineRanker::testRejectsLatencyBudgetExceeded()
{
    bs::OnlineRanker::TrainConfig cfg;
    cfg.promotionLatencyUsMax = 100.0;

    bs::OnlineRanker::TrainMetrics active;
    active.examples = 20;
    active.logLoss = 0.69;
    active.avgPredictionLatencyUs = 90.0;
    active.predictionFailureRate = 0.0;
    active.probabilitySaturationRate = 0.0;

    bs::OnlineRanker::TrainMetrics candidate;
    candidate.examples = 20;
    candidate.logLoss = 0.65;
    candidate.avgPredictionLatencyUs = 101.0;
    candidate.predictionFailureRate = 0.0;
    candidate.probabilitySaturationRate = 0.0;

    QString reason;
    const bool allowed = bs::OnlineRanker::passesPromotionRuntimeGates(cfg, active, candidate, &reason);
    QVERIFY(!allowed);
    QCOMPARE(reason, QStringLiteral("candidate_latency_budget_exceeded"));
}

void TestOnlineRanker::testRejectsLatencyRegressionExceeded()
{
    bs::OnlineRanker::TrainConfig cfg;
    cfg.promotionLatencyUsMax = 1000.0;
    cfg.promotionLatencyRegressionPctMax = 10.0;

    bs::OnlineRanker::TrainMetrics active;
    active.examples = 20;
    active.logLoss = 0.69;
    active.avgPredictionLatencyUs = 100.0;
    active.predictionFailureRate = 0.0;
    active.probabilitySaturationRate = 0.0;

    bs::OnlineRanker::TrainMetrics candidate;
    candidate.examples = 20;
    candidate.logLoss = 0.65;
    candidate.avgPredictionLatencyUs = 111.0;
    candidate.predictionFailureRate = 0.0;
    candidate.probabilitySaturationRate = 0.0;

    QString reason;
    const bool allowed = bs::OnlineRanker::passesPromotionRuntimeGates(cfg, active, candidate, &reason);
    QVERIFY(!allowed);
    QCOMPARE(reason, QStringLiteral("candidate_latency_regression_exceeded"));
}

void TestOnlineRanker::testRejectsFailureRateExceeded()
{
    bs::OnlineRanker::TrainConfig cfg;
    cfg.promotionLatencyUsMax = 1000.0;
    cfg.promotionLatencyRegressionPctMax = 50.0;
    cfg.promotionPredictionFailureRateMax = 0.02;

    bs::OnlineRanker::TrainMetrics active;
    active.examples = 20;
    active.logLoss = 0.69;
    active.avgPredictionLatencyUs = 100.0;
    active.predictionFailureRate = 0.0;
    active.probabilitySaturationRate = 0.0;

    bs::OnlineRanker::TrainMetrics candidate;
    candidate.examples = 20;
    candidate.logLoss = 0.65;
    candidate.avgPredictionLatencyUs = 100.0;
    candidate.predictionFailureRate = 0.03;
    candidate.probabilitySaturationRate = 0.0;

    QString reason;
    const bool allowed = bs::OnlineRanker::passesPromotionRuntimeGates(cfg, active, candidate, &reason);
    QVERIFY(!allowed);
    QCOMPARE(reason, QStringLiteral("candidate_stability_failure_rate_exceeded"));
}

void TestOnlineRanker::testRejectsSaturationRateExceeded()
{
    bs::OnlineRanker::TrainConfig cfg;
    cfg.promotionLatencyUsMax = 1000.0;
    cfg.promotionLatencyRegressionPctMax = 50.0;
    cfg.promotionPredictionFailureRateMax = 0.2;
    cfg.promotionSaturationRateMax = 0.4;

    bs::OnlineRanker::TrainMetrics active;
    active.examples = 20;
    active.logLoss = 0.69;
    active.avgPredictionLatencyUs = 100.0;
    active.predictionFailureRate = 0.0;
    active.probabilitySaturationRate = 0.0;

    bs::OnlineRanker::TrainMetrics candidate;
    candidate.examples = 20;
    candidate.logLoss = 0.65;
    candidate.avgPredictionLatencyUs = 100.0;
    candidate.predictionFailureRate = 0.0;
    candidate.probabilitySaturationRate = 0.41;

    QString reason;
    const bool allowed = bs::OnlineRanker::passesPromotionRuntimeGates(cfg, active, candidate, &reason);
    QVERIFY(!allowed);
    QCOMPARE(reason, QStringLiteral("candidate_stability_saturation_rate_exceeded"));
}

void TestOnlineRanker::testAcceptsHealthyCandidateMetrics()
{
    bs::OnlineRanker::TrainConfig cfg;
    cfg.promotionLatencyUsMax = 1000.0;
    cfg.promotionLatencyRegressionPctMax = 50.0;
    cfg.promotionPredictionFailureRateMax = 0.2;
    cfg.promotionSaturationRateMax = 0.9;

    bs::OnlineRanker::TrainMetrics active;
    active.examples = 20;
    active.logLoss = 0.69;
    active.avgPredictionLatencyUs = 100.0;
    active.predictionFailureRate = 0.0;
    active.probabilitySaturationRate = 0.0;

    bs::OnlineRanker::TrainMetrics candidate;
    candidate.examples = 20;
    candidate.logLoss = 0.65;
    candidate.avgPredictionLatencyUs = 120.0;
    candidate.predictionFailureRate = 0.01;
    candidate.probabilitySaturationRate = 0.1;

    QString reason;
    const bool allowed = bs::OnlineRanker::passesPromotionRuntimeGates(cfg, active, candidate, &reason);
    QVERIFY(allowed);
    QVERIFY(reason.isEmpty());
}

QTEST_MAIN(TestOnlineRanker)
#include "test_online_ranker.moc"
