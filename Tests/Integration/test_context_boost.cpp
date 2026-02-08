#include <QtTest/QtTest>
#include "core/ranking/scorer.h"
#include "core/shared/search_result.h"

class TestContextBoost : public QObject {
    Q_OBJECT

private slots:
    void testCwdBoostRanksCloserFiles();
    void testAppContextBoost();
};

void TestContextBoost::testCwdBoostRanksCloserFiles()
{
    bs::Scorer scorer;
    bs::QueryContext context;
    context.cwdPath = QStringLiteral("/Users/test/project");

    bs::SearchResult inside;
    inside.itemId = 1;
    inside.path = QStringLiteral("/Users/test/project/src/main.cpp");
    inside.name = QStringLiteral("main.cpp");
    inside.matchType = bs::MatchType::ContainsName;

    bs::SearchResult outside;
    outside.itemId = 2;
    outside.path = QStringLiteral("/Users/test/other/readme.md");
    outside.name = QStringLiteral("readme.md");
    outside.matchType = bs::MatchType::ContainsName;

    const bs::ScoreBreakdown insideScore = scorer.computeScore(inside, context);
    const bs::ScoreBreakdown outsideScore = scorer.computeScore(outside, context);

    QVERIFY(insideScore.contextBoost > outsideScore.contextBoost);
}

void TestContextBoost::testAppContextBoost()
{
    bs::Scorer scorer;
    bs::QueryContext context;
    context.frontmostAppBundleId = QStringLiteral("com.microsoft.VSCode");

    bs::SearchResult codeFile;
    codeFile.itemId = 1;
    codeFile.path = QStringLiteral("/Users/test/project/main.cpp");
    codeFile.name = QStringLiteral("main.cpp");
    codeFile.matchType = bs::MatchType::ContainsName;

    bs::SearchResult nonCodeFile;
    nonCodeFile.itemId = 2;
    nonCodeFile.path = QStringLiteral("/Users/test/project/design.pdf");
    nonCodeFile.name = QStringLiteral("design.pdf");
    nonCodeFile.matchType = bs::MatchType::ContainsName;

    const bs::ScoreBreakdown codeScore = scorer.computeScore(codeFile, context);
    const bs::ScoreBreakdown nonCodeScore = scorer.computeScore(nonCodeFile, context);

    QVERIFY(codeScore.contextBoost > nonCodeScore.contextBoost);
}

QTEST_MAIN(TestContextBoost)
#include "test_context_boost.moc"
