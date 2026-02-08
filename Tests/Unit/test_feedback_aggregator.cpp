#include <QtTest/QtTest>
#include "core/feedback/feedback_aggregator.h"

#include <sqlite3.h>

class TestFeedbackAggregator : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testRunAggregation();
    void testCleanup();
    void testLastAggregationTime();
    void testEmptyDatabase();

private:
    void resetTables();
    sqlite3* m_db = nullptr;
};

void TestFeedbackAggregator::initTestCase()
{
    QCOMPARE(sqlite3_open(":memory:", &m_db), SQLITE_OK);

    const char* schemaSql = R"(
        CREATE TABLE IF NOT EXISTS items (
            id INTEGER PRIMARY KEY,
            is_pinned INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS feedback (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            item_id INTEGER NOT NULL,
            action TEXT NOT NULL DEFAULT 'open',
            path TEXT NOT NULL DEFAULT '',
            open_count INTEGER NOT NULL DEFAULT 0,
            last_open TEXT,
            total_dwell_ms INTEGER NOT NULL DEFAULT 0,
            timestamp TEXT NOT NULL DEFAULT (datetime('now')),
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS interactions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            query TEXT NOT NULL DEFAULT '',
            item_id INTEGER NOT NULL DEFAULT 0,
            path TEXT NOT NULL DEFAULT '',
            timestamp TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS frequencies (
            item_id INTEGER PRIMARY KEY,
            open_count INTEGER NOT NULL DEFAULT 0,
            last_opened_at REAL,
            total_interactions INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL DEFAULT ''
        );
    )";
    QCOMPARE(sqlite3_exec(m_db, schemaSql, nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestFeedbackAggregator::cleanupTestCase()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void TestFeedbackAggregator::resetTables()
{
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM feedback;", nullptr, nullptr, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM interactions;", nullptr, nullptr, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM frequencies;", nullptr, nullptr, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM settings;", nullptr, nullptr, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM items;", nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestFeedbackAggregator::cleanup()
{
    resetTables();
}

void TestFeedbackAggregator::testRunAggregation()
{
    QCOMPARE(sqlite3_exec(m_db,
        "INSERT INTO items (id, is_pinned) VALUES (1, 0);"
        "INSERT INTO feedback (item_id, action, timestamp) VALUES (1, 'open', datetime('now'));"
        "INSERT INTO feedback (item_id, action, timestamp) VALUES (1, 'open', datetime('now'));",
        nullptr, nullptr, nullptr), SQLITE_OK);

    bs::FeedbackAggregator aggregator(m_db);
    QVERIFY(aggregator.runAggregation());

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM frequencies;", -1, &stmt, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    QVERIFY(count > 0);
}

void TestFeedbackAggregator::testCleanup()
{
    QCOMPARE(sqlite3_exec(m_db,
        "INSERT INTO feedback (item_id, action, timestamp) VALUES (2, 'open', datetime('now', '-200 days'));"
        "INSERT INTO interactions (query, item_id, path, timestamp) VALUES ('x', 2, '/tmp/a', datetime('now', '-200 days'));",
        nullptr, nullptr, nullptr), SQLITE_OK);

    bs::FeedbackAggregator aggregator(m_db);
    QVERIFY(aggregator.cleanup(90, 180));

    sqlite3_stmt* feedbackStmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM feedback;", -1, &feedbackStmt, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_step(feedbackStmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(feedbackStmt, 0), 0);
    sqlite3_finalize(feedbackStmt);

    sqlite3_stmt* interactionStmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM interactions;", -1, &interactionStmt, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_step(interactionStmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(interactionStmt, 0), 0);
    sqlite3_finalize(interactionStmt);
}

void TestFeedbackAggregator::testLastAggregationTime()
{
    QCOMPARE(sqlite3_exec(m_db,
        "INSERT INTO items (id, is_pinned) VALUES (5, 0);"
        "INSERT INTO feedback (item_id, action, timestamp) VALUES (5, 'open', datetime('now'));",
        nullptr, nullptr, nullptr), SQLITE_OK);

    bs::FeedbackAggregator aggregator(m_db);
    QVERIFY(aggregator.runAggregation());

    const QDateTime last = aggregator.lastAggregationTime();
    QVERIFY(last.isValid());
    QVERIFY(last.secsTo(QDateTime::currentDateTimeUtc()) < 60);
}

void TestFeedbackAggregator::testEmptyDatabase()
{
    bs::FeedbackAggregator aggregator(m_db);
    QVERIFY(aggregator.runAggregation());

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM frequencies;", -1, &stmt, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
}

QTEST_MAIN(TestFeedbackAggregator)
#include "test_feedback_aggregator.moc"
