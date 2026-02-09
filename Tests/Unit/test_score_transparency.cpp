#include <QtTest/QtTest>
#include "core/ranking/scorer.h"
#include "core/shared/scoring_types.h"
#include "core/shared/search_result.h"

class TestScoreTransparency : public QObject {
    Q_OBJECT

private:
    bs::SearchResult makeResult(int64_t id, const QString& path,
                                const QString& name, bs::MatchType matchType) const
    {
        bs::SearchResult r;
        r.itemId = id;
        r.path = path;
        r.name = name;
        r.matchType = matchType;
        return r;
    }

private slots:
    void testFeedbackBoostInBreakdown()
    {
        // Verify the feedbackBoost field exists and is additive
        bs::ScoreBreakdown bd;
        bd.feedbackBoost = 5.0;
        QCOMPARE(bd.feedbackBoost, 5.0);

        // When set on a result, Scorer should include it in the final score
        bs::ScoringWeights weights;
        bs::Scorer scorer(weights);

        bs::SearchResult r = makeResult(1, "/test/file.txt", "file.txt",
                                         bs::MatchType::ExactName);
        r.scoreBreakdown.feedbackBoost = 10.0;

        std::vector<bs::SearchResult> results = {r};
        bs::QueryContext ctx;
        scorer.rankResults(results, ctx);

        // feedbackBoost should be part of the final score
        QVERIFY(results[0].score >= 10.0);
    }

    void testScoreBreakdownSumsCorrectly()
    {
        bs::ScoringWeights weights;
        bs::Scorer scorer(weights);

        bs::SearchResult r = makeResult(1, "/test/report.pdf", "report.pdf",
                                         bs::MatchType::PrefixName);
        r.scoreBreakdown.feedbackBoost = 3.0;
        r.scoreBreakdown.m2SignalBoost = 7.0;

        std::vector<bs::SearchResult> results = {r};
        bs::QueryContext ctx;
        scorer.rankResults(results, ctx);

        const auto& bd = results[0].scoreBreakdown;
        const double expectedSum = bd.baseMatchScore + bd.recencyBoost
                                   + bd.frequencyBoost + bd.contextBoost
                                   + bd.pinnedBoost + bd.semanticBoost
                                   + bd.crossEncoderBoost + bd.structuredQueryBoost
                                   + bd.feedbackBoost + bd.m2SignalBoost
                                   - bd.junkPenalty;
        const double finalScore = std::max(0.0, expectedSum);
        QCOMPARE(results[0].score, finalScore);
    }

    void testM2SignalBoostSeparate()
    {
        // Verify m2SignalBoost is a distinct field from feedbackBoost
        bs::ScoreBreakdown bd;
        QCOMPARE(bd.feedbackBoost, 0.0);
        QCOMPARE(bd.m2SignalBoost, 0.0);

        bd.feedbackBoost = 2.5;
        bd.m2SignalBoost = 4.5;
        QVERIFY(bd.feedbackBoost != bd.m2SignalBoost);

        // Both contribute independently to scorer
        bs::ScoringWeights weights;
        bs::Scorer scorer(weights);

        bs::SearchResult r1 = makeResult(1, "/a.txt", "a.txt", bs::MatchType::Content);
        r1.scoreBreakdown.feedbackBoost = 5.0;
        r1.scoreBreakdown.m2SignalBoost = 0.0;

        bs::SearchResult r2 = makeResult(2, "/b.txt", "b.txt", bs::MatchType::Content);
        r2.scoreBreakdown.feedbackBoost = 0.0;
        r2.scoreBreakdown.m2SignalBoost = 5.0;

        std::vector<bs::SearchResult> results1 = {r1};
        std::vector<bs::SearchResult> results2 = {r2};
        bs::QueryContext ctx;
        scorer.rankResults(results1, ctx);
        scorer.rankResults(results2, ctx);

        // Both should get the same boost from their respective 5.0 contribution
        QCOMPARE(results1[0].score, results2[0].score);
    }

    void testDefaultFieldsAreZero()
    {
        bs::ScoreBreakdown bd;
        QCOMPARE(bd.feedbackBoost, 0.0);
        QCOMPARE(bd.m2SignalBoost, 0.0);
    }
};

QTEST_MAIN(TestScoreTransparency)
#include "test_score_transparency.moc"
