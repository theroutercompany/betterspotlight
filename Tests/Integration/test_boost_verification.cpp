#include <QtTest/QtTest>
#include "core/feedback/interaction_tracker.h"
#include "core/shared/search_result.h"

#include <sqlite3.h>

#include <algorithm>

class TestBoostVerification : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testInteractionBoostAffectsRanking();

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

QTEST_MAIN(TestBoostVerification)
#include "test_boost_verification.moc"
