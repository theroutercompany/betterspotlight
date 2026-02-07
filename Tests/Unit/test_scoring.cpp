#include <QtTest/QtTest>
#include "core/ranking/scorer.h"
#include "core/shared/scoring_types.h"
#include "core/shared/search_result.h"

#include <QDateTime>
#include <cmath>

class TestScoring : public QObject {
    Q_OBJECT

private:
    // Helper to create a minimal SearchResult
    bs::SearchResult makeResult(int64_t id, const QString& path,
                                const QString& name, bs::MatchType matchType,
                                bool isPinned = false, int openCount = 0,
                                const QString& modDate = {},
                                const QString& lastOpenDate = {}) const
    {
        bs::SearchResult r;
        r.itemId = id;
        r.path = path;
        r.name = name;
        r.matchType = matchType;
        r.isPinned = isPinned;
        r.openCount = openCount;
        r.modificationDate = modDate;
        r.lastOpenDate = lastOpenDate;
        return r;
    }

private slots:
    // ── Match type ordering ──────────────────────────────────────
    void testExactNameHigherThanPrefixName();
    void testPrefixNameHigherThanContainsName();
    void testContainsNameHigherThanExactPath();
    void testExactPathHigherThanPrefixPath();
    void testContentHigherThanFuzzy();
    void testMatchTypeFullOrdering();

    // ── Recency boost ────────────────────────────────────────────
    void testRecentFilesScoreHigher();
    void testVeryOldFileMinimalRecencyBoost();
    void testFutureModTimeGivesFullBoost();

    // ── Frequency boost ──────────────────────────────────────────
    void testFrequencyBoostTier1();
    void testFrequencyBoostTier2();
    void testFrequencyBoostTier3();
    void testFrequencyBoostZeroOpens();

    // ── Pinned boost ─────────────────────────────────────────────
    void testPinnedBoostApplied();
    void testNotPinnedNoBoost();

    // ── Junk penalty ─────────────────────────────────────────────
    void testJunkPenaltyNodeModules();
    void testJunkPenaltyPycache();
    void testJunkPenaltyGitDir();
    void testNoJunkPenaltyNormalPath();

    // ── rankResults sorting ──────────────────────────────────────
    void testRankResultsSortsByScoreDescending();
    void testRankResultsTiesBreakByItemIdAscending();

    // ── Custom weights ───────────────────────────────────────────
    void testCustomScoringWeights();

    // ── Context signals ──────────────────────────────────────────
    void testCwdProximityBoost();
};

// ── Match type ordering ──────────────────────────────────────────

void TestScoring::testExactNameHigherThanPrefixName()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    auto exact = makeResult(1, "/a/readme.md", "readme.md", bs::MatchType::ExactName);
    auto prefix = makeResult(2, "/a/readme.md", "readme.md", bs::MatchType::PrefixName);

    auto s1 = scorer.computeScore(exact, ctx);
    auto s2 = scorer.computeScore(prefix, ctx);

    QVERIFY(s1.baseMatchScore > s2.baseMatchScore);
}

void TestScoring::testPrefixNameHigherThanContainsName()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    auto prefix = makeResult(1, "/a/f.txt", "f.txt", bs::MatchType::PrefixName);
    auto contains = makeResult(2, "/a/f.txt", "f.txt", bs::MatchType::ContainsName);

    auto s1 = scorer.computeScore(prefix, ctx);
    auto s2 = scorer.computeScore(contains, ctx);

    QVERIFY(s1.baseMatchScore > s2.baseMatchScore);
}

void TestScoring::testContainsNameHigherThanExactPath()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    auto contains = makeResult(1, "/a/f.txt", "f.txt", bs::MatchType::ContainsName);
    auto exactPath = makeResult(2, "/a/f.txt", "f.txt", bs::MatchType::ExactPath);

    auto s1 = scorer.computeScore(contains, ctx);
    auto s2 = scorer.computeScore(exactPath, ctx);

    QVERIFY(s1.baseMatchScore > s2.baseMatchScore);
}

void TestScoring::testExactPathHigherThanPrefixPath()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    auto exactPath = makeResult(1, "/a/f.txt", "f.txt", bs::MatchType::ExactPath);
    auto prefixPath = makeResult(2, "/a/f.txt", "f.txt", bs::MatchType::PrefixPath);

    auto s1 = scorer.computeScore(exactPath, ctx);
    auto s2 = scorer.computeScore(prefixPath, ctx);

    QVERIFY(s1.baseMatchScore > s2.baseMatchScore);
}

void TestScoring::testContentHigherThanFuzzy()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    // Content with a reasonable BM25 score
    auto content = makeResult(1, "/a/f.txt", "f.txt", bs::MatchType::Content);
    auto fuzzy = makeResult(2, "/a/f.txt", "f.txt", bs::MatchType::Fuzzy);

    // For Content, the base score is bm25 * contentMatchWeight
    // With bm25=50, that gives 50 > Fuzzy's 30
    auto s1 = scorer.computeScore(content, ctx, 50.0);
    auto s2 = scorer.computeScore(fuzzy, ctx);

    QVERIFY(s1.baseMatchScore > s2.baseMatchScore);
}

void TestScoring::testMatchTypeFullOrdering()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    auto exact = scorer.computeScore(
        makeResult(1, "/a", "a", bs::MatchType::ExactName), ctx);
    auto prefix = scorer.computeScore(
        makeResult(2, "/a", "a", bs::MatchType::PrefixName), ctx);
    auto contains = scorer.computeScore(
        makeResult(3, "/a", "a", bs::MatchType::ContainsName), ctx);
    auto exactPath = scorer.computeScore(
        makeResult(4, "/a", "a", bs::MatchType::ExactPath), ctx);
    auto prefixPath = scorer.computeScore(
        makeResult(5, "/a", "a", bs::MatchType::PrefixPath), ctx);
    auto fuzzy = scorer.computeScore(
        makeResult(7, "/a", "a", bs::MatchType::Fuzzy), ctx);

    // Verify: ExactName(200) > PrefixName(150) > ContainsName(100) > ExactPath(90) > PrefixPath(80) > Fuzzy(30)
    QVERIFY(exact.baseMatchScore > prefix.baseMatchScore);
    QVERIFY(prefix.baseMatchScore > contains.baseMatchScore);
    QVERIFY(contains.baseMatchScore > exactPath.baseMatchScore);
    QVERIFY(exactPath.baseMatchScore > prefixPath.baseMatchScore);
    QVERIFY(prefixPath.baseMatchScore > fuzzy.baseMatchScore);

    // Verify specific values
    QCOMPARE(exact.baseMatchScore, 200.0);
    QCOMPARE(prefix.baseMatchScore, 150.0);
    QCOMPARE(contains.baseMatchScore, 100.0);
    QCOMPARE(exactPath.baseMatchScore, 90.0);
    QCOMPARE(prefixPath.baseMatchScore, 80.0);
    QCOMPARE(fuzzy.baseMatchScore, 30.0);
}

// ── Recency boost ────────────────────────────────────────────────

void TestScoring::testRecentFilesScoreHigher()
{
    bs::Scorer scorer;
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const double oneHourAgo = now - 3600.0;
    const double oneMonthAgo = now - 30.0 * 86400.0;

    double recentBoost = scorer.computeRecencyBoost(oneHourAgo);
    double oldBoost = scorer.computeRecencyBoost(oneMonthAgo);

    QVERIFY(recentBoost > oldBoost);
    QVERIFY(recentBoost > 0.0);
    QVERIFY(oldBoost >= 0.0);
}

void TestScoring::testVeryOldFileMinimalRecencyBoost()
{
    bs::Scorer scorer;
    const double veryOld = 946684800.0; // 2000-01-01
    double boost = scorer.computeRecencyBoost(veryOld);
    QVERIFY(boost < 1.0); // Nearly zero boost
}

void TestScoring::testFutureModTimeGivesFullBoost()
{
    bs::Scorer scorer;
    const double future = static_cast<double>(QDateTime::currentSecsSinceEpoch()) + 86400.0;
    double boost = scorer.computeRecencyBoost(future);
    // Future files get full recency weight
    QCOMPARE(boost, static_cast<double>(scorer.weights().recencyWeight));
}

// ── Frequency boost ──────────────────────────────────────────────

void TestScoring::testFrequencyBoostTier1()
{
    bs::Scorer scorer;
    double boost = scorer.computeFrequencyBoost(3); // 1-5 opens
    QCOMPARE(boost, static_cast<double>(scorer.weights().frequencyTier1Boost));
}

void TestScoring::testFrequencyBoostTier2()
{
    bs::Scorer scorer;
    double boost = scorer.computeFrequencyBoost(10); // 6-20 opens
    QCOMPARE(boost, static_cast<double>(scorer.weights().frequencyTier2Boost));
}

void TestScoring::testFrequencyBoostTier3()
{
    bs::Scorer scorer;
    double boost = scorer.computeFrequencyBoost(25); // 21+ opens
    QCOMPARE(boost, static_cast<double>(scorer.weights().frequencyTier3Boost));
}

void TestScoring::testFrequencyBoostZeroOpens()
{
    bs::Scorer scorer;
    double boost = scorer.computeFrequencyBoost(0);
    QCOMPARE(boost, 0.0);
}

// ── Pinned boost ─────────────────────────────────────────────────

void TestScoring::testPinnedBoostApplied()
{
    bs::Scorer scorer;
    double boost = scorer.computePinnedBoost(true);
    QCOMPARE(boost, static_cast<double>(scorer.weights().pinnedBoostWeight));
}

void TestScoring::testNotPinnedNoBoost()
{
    bs::Scorer scorer;
    double boost = scorer.computePinnedBoost(false);
    QCOMPARE(boost, 0.0);
}

// ── Junk penalty ─────────────────────────────────────────────────

void TestScoring::testJunkPenaltyNodeModules()
{
    bs::Scorer scorer;
    double penalty = scorer.computeJunkPenalty(
        QStringLiteral("/Users/me/project/node_modules/express/index.js"));
    QCOMPARE(penalty, static_cast<double>(scorer.weights().junkPenaltyWeight));
}

void TestScoring::testJunkPenaltyPycache()
{
    bs::Scorer scorer;
    double penalty = scorer.computeJunkPenalty(
        QStringLiteral("/Users/me/project/__pycache__/module.pyc"));
    QCOMPARE(penalty, static_cast<double>(scorer.weights().junkPenaltyWeight));
}

void TestScoring::testJunkPenaltyGitDir()
{
    bs::Scorer scorer;
    double penalty = scorer.computeJunkPenalty(
        QStringLiteral("/Users/me/project/.git/config"));
    QCOMPARE(penalty, static_cast<double>(scorer.weights().junkPenaltyWeight));
}

void TestScoring::testNoJunkPenaltyNormalPath()
{
    bs::Scorer scorer;
    double penalty = scorer.computeJunkPenalty(
        QStringLiteral("/Users/me/Documents/report.txt"));
    QCOMPARE(penalty, 0.0);
}

// ── rankResults ──────────────────────────────────────────────────

void TestScoring::testRankResultsSortsByScoreDescending()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    std::vector<bs::SearchResult> results;
    results.push_back(makeResult(1, "/a/f.txt", "f.txt", bs::MatchType::Fuzzy));
    results.push_back(makeResult(2, "/a/readme.md", "readme.md", bs::MatchType::ExactName));
    results.push_back(makeResult(3, "/a/g.txt", "g.txt", bs::MatchType::ContainsName));

    scorer.rankResults(results, ctx);

    // ExactName(200) > ContainsName(100) > Fuzzy(30)
    QCOMPARE(results[0].itemId, static_cast<int64_t>(2)); // ExactName
    QCOMPARE(results[1].itemId, static_cast<int64_t>(3)); // ContainsName
    QCOMPARE(results[2].itemId, static_cast<int64_t>(1)); // Fuzzy
}

void TestScoring::testRankResultsTiesBreakByItemIdAscending()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;

    std::vector<bs::SearchResult> results;
    // Same match type -> same base score, different IDs
    results.push_back(makeResult(5, "/a/e.txt", "e.txt", bs::MatchType::ContainsName));
    results.push_back(makeResult(2, "/a/b.txt", "b.txt", bs::MatchType::ContainsName));
    results.push_back(makeResult(8, "/a/h.txt", "h.txt", bs::MatchType::ContainsName));

    scorer.rankResults(results, ctx);

    // Same score => tie-break by itemId ascending
    QCOMPARE(results[0].itemId, static_cast<int64_t>(2));
    QCOMPARE(results[1].itemId, static_cast<int64_t>(5));
    QCOMPARE(results[2].itemId, static_cast<int64_t>(8));
}

// ── Custom weights ───────────────────────────────────────────────

void TestScoring::testCustomScoringWeights()
{
    bs::ScoringWeights custom;
    custom.exactNameWeight = 500;
    custom.fuzzyMatchWeight = 10;
    custom.junkPenaltyWeight = 100;
    custom.pinnedBoostWeight = 300;

    bs::Scorer scorer(custom);
    bs::QueryContext ctx;

    auto exact = scorer.computeScore(
        makeResult(1, "/a", "a", bs::MatchType::ExactName), ctx);
    auto fuzzy = scorer.computeScore(
        makeResult(2, "/a", "a", bs::MatchType::Fuzzy), ctx);

    QCOMPARE(exact.baseMatchScore, 500.0);
    QCOMPARE(fuzzy.baseMatchScore, 10.0);

    QCOMPARE(scorer.computePinnedBoost(true), 300.0);
    QCOMPARE(scorer.computeJunkPenalty(QStringLiteral("/x/node_modules/y")), 100.0);
}

// ── Context signals ──────────────────────────────────────────────

void TestScoring::testCwdProximityBoost()
{
    bs::Scorer scorer;
    bs::QueryContext ctx;
    ctx.cwdPath = QStringLiteral("/Users/me/project");

    // File directly inside CWD should get a boost
    auto r1 = makeResult(1, "/Users/me/project/main.cpp", "main.cpp",
                          bs::MatchType::ContainsName);
    auto s1 = scorer.computeScore(r1, ctx);
    QVERIFY(s1.contextBoost > 0.0);

    // File far away from CWD should get no boost
    auto r2 = makeResult(2, "/Users/other/Documents/file.txt", "file.txt",
                          bs::MatchType::ContainsName);
    auto s2 = scorer.computeScore(r2, ctx);
    QCOMPARE(s2.contextBoost, 0.0);
}

QTEST_MAIN(TestScoring)
#include "test_scoring.moc"
