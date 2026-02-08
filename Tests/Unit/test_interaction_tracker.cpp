#include <QtTest/QtTest>
#include "core/feedback/interaction_tracker.h"

#include <sqlite3.h>

class TestInteractionTracker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testRecordAndRetrieve();
    void testBoostCalculation();
    void testBoostCapAt25();
    void testZeroBoostForUnknown();
    void testNormalizeQuery();
    void testCleanup();
    void testExportData();

private:
    void resetTables();

    sqlite3* m_db = nullptr;
};

void TestInteractionTracker::initTestCase()
{
    const int rc = sqlite3_open(":memory:", &m_db);
    QCOMPARE(rc, SQLITE_OK);

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS interactions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            query TEXT NOT NULL,
            query_normalized TEXT NOT NULL DEFAULT '',
            item_id INTEGER NOT NULL DEFAULT 0,
            path TEXT NOT NULL DEFAULT '',
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

void TestInteractionTracker::cleanupTestCase()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void TestInteractionTracker::resetTables()
{
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM interactions;", nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestInteractionTracker::cleanup()
{
    resetTables();
}

void TestInteractionTracker::testRecordAndRetrieve()
{
    bs::InteractionTracker tracker(m_db);

    bs::InteractionTracker::Interaction interaction;
    interaction.query = QStringLiteral("hello world");
    interaction.selectedItemId = 77;
    interaction.selectedPath = QStringLiteral("/tmp/file.txt");
    interaction.matchType = QStringLiteral("contains_name");
    interaction.resultPosition = 1;
    interaction.frontmostApp = QStringLiteral("Code");
    interaction.timestamp = QDateTime::currentDateTimeUtc();

    QVERIFY(tracker.recordInteraction(interaction));
    QCOMPARE(tracker.getInteractionCount(QStringLiteral("hello world"), 77), 1);
}

void TestInteractionTracker::testBoostCalculation()
{
    bs::InteractionTracker tracker(m_db);
    bs::InteractionTracker::Interaction interaction;
    interaction.query = QStringLiteral("boost me");
    interaction.selectedItemId = 5;
    interaction.selectedPath = QStringLiteral("/tmp/boost.txt");
    interaction.timestamp = QDateTime::currentDateTimeUtc();

    for (int i = 0; i < 5; ++i) {
        QVERIFY(tracker.recordInteraction(interaction));
    }

    QCOMPARE(tracker.getInteractionBoost(QStringLiteral("boost me"), 5), 25);
}

void TestInteractionTracker::testBoostCapAt25()
{
    bs::InteractionTracker tracker(m_db);
    bs::InteractionTracker::Interaction interaction;
    interaction.query = QStringLiteral("cap");
    interaction.selectedItemId = 9;
    interaction.selectedPath = QStringLiteral("/tmp/cap.txt");
    interaction.timestamp = QDateTime::currentDateTimeUtc();

    for (int i = 0; i < 10; ++i) {
        QVERIFY(tracker.recordInteraction(interaction));
    }

    QVERIFY(tracker.getInteractionBoost(QStringLiteral("cap"), 9) <= 25);
}

void TestInteractionTracker::testZeroBoostForUnknown()
{
    bs::InteractionTracker tracker(m_db);
    QCOMPARE(tracker.getInteractionBoost(QStringLiteral("missing"), 1234), 0);
}

void TestInteractionTracker::testNormalizeQuery()
{
    const QString normalized = bs::InteractionTracker::normalizeQuery(QStringLiteral(" Hello  World "));
    QCOMPARE(normalized, QStringLiteral("hello world"));
}

void TestInteractionTracker::testCleanup()
{
    bs::InteractionTracker tracker(m_db);

    bs::InteractionTracker::Interaction interaction;
    interaction.query = QStringLiteral("old");
    interaction.selectedItemId = 12;
    interaction.selectedPath = QStringLiteral("/tmp/old.txt");
    interaction.timestamp = QDateTime::currentDateTimeUtc().addDays(-2);
    QVERIFY(tracker.recordInteraction(interaction));

    QVERIFY(tracker.cleanup(0));
    QCOMPARE(tracker.getInteractionCount(QStringLiteral("old"), 12), 0);
}

void TestInteractionTracker::testExportData()
{
    bs::InteractionTracker tracker(m_db);

    bs::InteractionTracker::Interaction interaction;
    interaction.query = QStringLiteral("export");
    interaction.selectedItemId = 90;
    interaction.selectedPath = QStringLiteral("/tmp/export.txt");
    interaction.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(tracker.recordInteraction(interaction));

    const QJsonArray data = tracker.exportData();
    QVERIFY(!data.isEmpty());
}

QTEST_MAIN(TestInteractionTracker)
#include "test_interaction_tracker.moc"
