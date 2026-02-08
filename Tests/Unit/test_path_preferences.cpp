#include <QtTest/QtTest>
#include "core/feedback/path_preferences.h"

#include <sqlite3.h>

class TestPathPreferences : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testEmptyReturnsZeroBoost();
    void testBoostCalculation();
    void testCacheInvalidation();
    void testGetTopDirectories();
    void testTopDirectoriesLimit();
    void testBoostFormula();

private:
    sqlite3* m_db = nullptr;
};

void TestPathPreferences::initTestCase()
{
    QCOMPARE(sqlite3_open(":memory:", &m_db), SQLITE_OK);
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS interactions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            query TEXT NOT NULL DEFAULT '',
            selected_item_id INTEGER NOT NULL DEFAULT 0,
            selected_path TEXT NOT NULL DEFAULT '',
            item_id INTEGER NOT NULL DEFAULT 0,
            path TEXT NOT NULL DEFAULT '',
            match_type TEXT NOT NULL DEFAULT '',
            result_position INTEGER NOT NULL DEFAULT 0,
            frontmost_app TEXT NOT NULL DEFAULT '',
            app_context TEXT,
            timestamp TEXT NOT NULL DEFAULT (datetime('now')),
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
    )";
    QCOMPARE(sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestPathPreferences::cleanupTestCase()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void TestPathPreferences::cleanup()
{
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM interactions;", nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestPathPreferences::testEmptyReturnsZeroBoost()
{
    bs::PathPreferences preferences(m_db);
    QCOMPARE(preferences.getBoost(QStringLiteral("/tmp/empty/file.txt")), 0.0);
}

void TestPathPreferences::testBoostCalculation()
{
    for (int i = 0; i < 10; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (path, timestamp) VALUES ('/work/project/src/main.cpp', datetime('now'));",
            nullptr,
            nullptr,
            nullptr),
            SQLITE_OK);
    }

    bs::PathPreferences preferences(m_db);
    const double boost = preferences.getBoost(QStringLiteral("/work/project/src/other.cpp"));
    QCOMPARE(boost, 2.0);
}

void TestPathPreferences::testCacheInvalidation()
{
    bs::PathPreferences preferences(m_db);
    QCOMPARE(preferences.getBoost(QStringLiteral("/cache/test/a.cpp")), 0.0);

    for (int i = 0; i < 15; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (path, timestamp) VALUES ('/cache/test/a.cpp', datetime('now'));",
            nullptr,
            nullptr,
            nullptr),
            SQLITE_OK);
    }

    const double stale = preferences.getBoost(QStringLiteral("/cache/test/b.cpp"));
    QCOMPARE(stale, 0.0);

    preferences.invalidateCache();
    const double refreshed = preferences.getBoost(QStringLiteral("/cache/test/b.cpp"));
    QVERIFY(refreshed > 0.0);
}

void TestPathPreferences::testGetTopDirectories()
{
    QCOMPARE(sqlite3_exec(
        m_db,
        "INSERT INTO interactions (path, timestamp) VALUES ('/a/x/file1.txt', datetime('now'));"
        "INSERT INTO interactions (path, timestamp) VALUES ('/a/x/file2.txt', datetime('now'));"
        "INSERT INTO interactions (path, timestamp) VALUES ('/a/x/file3.txt', datetime('now'));"
        "INSERT INTO interactions (path, timestamp) VALUES ('/b/y/file1.txt', datetime('now'));",
        nullptr,
        nullptr,
        nullptr),
        SQLITE_OK);

    bs::PathPreferences preferences(m_db);
    const QVector<bs::PathPreferences::DirPreference> dirs = preferences.getTopDirectories(10);
    QVERIFY(!dirs.isEmpty());
    QVERIFY(dirs[0].selectionCount >= dirs.last().selectionCount);
}

void TestPathPreferences::testTopDirectoriesLimit()
{
    for (int d = 0; d < 20; ++d) {
        const QString sql = QString("INSERT INTO interactions (path, timestamp) VALUES ('/dir%1/file.txt', datetime('now'));").arg(d);
        QCOMPARE(sqlite3_exec(m_db, sql.toUtf8().constData(), nullptr, nullptr, nullptr), SQLITE_OK);
    }

    bs::PathPreferences preferences(m_db);
    const QVector<bs::PathPreferences::DirPreference> limited = preferences.getTopDirectories(5);
    QVERIFY(limited.size() <= 5);
}

void TestPathPreferences::testBoostFormula()
{
    for (int i = 0; i < 100; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (path, timestamp) VALUES ('/heavy/dir/file.cpp', datetime('now'));",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }

    bs::PathPreferences preferences(m_db);
    const double boost = preferences.getBoost(QStringLiteral("/heavy/dir/other.cpp"));
    QVERIFY(boost > 0.0);
    QVERIFY(boost <= 15.0);
}

QTEST_MAIN(TestPathPreferences)
#include "test_path_preferences.moc"
