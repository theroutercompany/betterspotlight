#include <QtTest/QtTest>
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/path_preferences.h"
#include "core/ranking/scorer.h"
#include "core/shared/search_result.h"
#include "core/shared/scoring_types.h"

#include <sqlite3.h>

#include <algorithm>

class TestBoostVerification : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testFrequencyBoost();
    void testInteractionBoostAffectsRanking();
    void testPathPreferenceBoost();

private:
    sqlite3* m_db = nullptr;
};

void TestBoostVerification::initTestCase()
{
    QCOMPARE(sqlite3_open(":memory:", &m_db), SQLITE_OK);
    const char* sql = R"(
        CREATE TABLE interactions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            query TEXT NOT NULL,
            query_normalized TEXT NOT NULL DEFAULT '',
            item_id INTEGER NOT NULL,
            path TEXT NOT NULL,
            match_type TEXT NOT NULL DEFAULT '',
            result_position INTEGER NOT NULL DEFAULT 0,
            app_context TEXT,
            timestamp TEXT NOT NULL DEFAULT (datetime('now')),
            selected_item_id INTEGER NOT NULL DEFAULT 0,
            selected_path TEXT NOT NULL DEFAULT '',
            frontmost_app TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
    )";
    QCOMPARE(sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestBoostVerification::cleanupTestCase()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void TestBoostVerification::cleanup()
{
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM interactions;", nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestBoostVerification::testFrequencyBoost()
{
    bs::Scorer scorer;

    bs::SearchResult frequentResult;
    frequentResult.itemId = 1;
    frequentResult.path = QStringLiteral("/tmp/frequent.cpp");
    frequentResult.name = QStringLiteral("frequent.cpp");
    frequentResult.matchType = bs::MatchType::ContainsName;
    frequentResult.openCount = 10;

    bs::SearchResult rareResult;
    rareResult.itemId = 2;
    rareResult.path = QStringLiteral("/tmp/rare.cpp");
    rareResult.name = QStringLiteral("rare.cpp");
    rareResult.matchType = bs::MatchType::ContainsName;
    rareResult.openCount = 0;

    bs::QueryContext ctx;
    const bs::ScoreBreakdown freqScore = scorer.computeScore(frequentResult, ctx);
    const bs::ScoreBreakdown rareScore = scorer.computeScore(rareResult, ctx);

    QVERIFY(freqScore.frequencyBoost > rareScore.frequencyBoost);
}

void TestBoostVerification::testInteractionBoostAffectsRanking()
{
    bs::InteractionTracker tracker(m_db);

    bs::InteractionTracker::Interaction interaction;
    interaction.query = QStringLiteral("query");
    interaction.selectedItemId = 101;
    interaction.selectedPath = QStringLiteral("/tmp/boosted.cpp");
    interaction.timestamp = QDateTime::currentDateTimeUtc();

    for (int i = 0; i < 5; ++i) {
        QVERIFY(tracker.recordInteraction(interaction));
    }

    const int boost = tracker.getInteractionBoost(QStringLiteral("query"), 101);
    QVERIFY(boost > 0);

    bs::SearchResult boosted;
    boosted.itemId = 101;
    boosted.score = 10.0;

    bs::SearchResult baseline;
    baseline.itemId = 202;
    baseline.score = 20.0;

    boosted.score += static_cast<double>(boost);

    std::vector<bs::SearchResult> ranked = {baseline, boosted};
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const bs::SearchResult& a, const bs::SearchResult& b) {
            return a.score > b.score;
        });

    QCOMPARE(ranked.front().itemId, static_cast<int64_t>(101));
}

void TestBoostVerification::testPathPreferenceBoost()
{
    for (int i = 0; i < 15; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (query, query_normalized, item_id, path, match_type, "
            "result_position, timestamp) VALUES ('test', 'test', 50, '/proj/src/main.cpp', "
            "'contains_name', 0, datetime('now'));",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }

    bs::PathPreferences prefs(m_db);
    const double boost = prefs.getBoost(QStringLiteral("/proj/src/other.cpp"));
    QVERIFY(boost > 0.0);

    const double noBoost = prefs.getBoost(QStringLiteral("/unrelated/dir/file.txt"));
    QVERIFY(boost > noBoost);
}

QTEST_MAIN(TestBoostVerification)
#include "test_boost_verification.moc"
