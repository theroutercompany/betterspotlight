#include <QtTest/QtTest>
#include "core/feedback/type_affinity.h"

#include <sqlite3.h>

class TestTypeAffinity : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testDefaultNoAffinity();
    void testCodeAffinity();
    void testCacheRefresh();
    void testExtensionMatching();

private:
    sqlite3* m_db = nullptr;
};

void TestTypeAffinity::initTestCase()
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

void TestTypeAffinity::cleanupTestCase()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void TestTypeAffinity::cleanup()
{
    QCOMPARE(sqlite3_exec(m_db, "DELETE FROM interactions;", nullptr, nullptr, nullptr), SQLITE_OK);
}

void TestTypeAffinity::testDefaultNoAffinity()
{
    bs::TypeAffinity affinity(m_db);
    QCOMPARE(affinity.getBoost(QStringLiteral("/tmp/code/main.cpp")), 0.0);
}

void TestTypeAffinity::testCodeAffinity()
{
    for (int i = 0; i < 30; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (path, timestamp) VALUES ('/proj/src/file.cpp', datetime('now'));",
            nullptr,
            nullptr,
            nullptr),
            SQLITE_OK);
    }

    bs::TypeAffinity affinity(m_db);
    QCOMPARE(affinity.getBoost(QStringLiteral("/another/path/thing.cpp")), 5.0);
}

void TestTypeAffinity::testCacheRefresh()
{
    bs::TypeAffinity affinity(m_db);
    QCOMPARE(affinity.getBoost(QStringLiteral("/cache/test/sample.cpp")), 0.0);

    for (int i = 0; i < 12; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (path, timestamp) VALUES ('/cache/test/new.cpp', datetime('now'));",
            nullptr,
            nullptr,
            nullptr),
            SQLITE_OK);
    }

    QCOMPARE(affinity.getBoost(QStringLiteral("/cache/test/new.cpp")), 0.0);
    affinity.invalidateCache();
    QVERIFY(affinity.getBoost(QStringLiteral("/cache/test/new.cpp")) > 0.0);
}

void TestTypeAffinity::testExtensionMatching()
{
    for (int i = 0; i < 20; ++i) {
        QCOMPARE(sqlite3_exec(
            m_db,
            "INSERT INTO interactions (path, timestamp) VALUES ('/code/seed.cpp', datetime('now'));",
            nullptr,
            nullptr,
            nullptr),
            SQLITE_OK);
    }

    bs::TypeAffinity affinity(m_db);
    affinity.invalidateCache();

    const QStringList codeFiles = {
        QStringLiteral("/a/main.cpp"),
        QStringLiteral("/a/header.h"),
        QStringLiteral("/a/script.py"),
        QStringLiteral("/a/app.ts"),
        QStringLiteral("/a/server.go"),
        QStringLiteral("/a/lib.rs"),
        QStringLiteral("/a/index.js"),
    };

    for (const QString& file : codeFiles) {
        QCOMPARE(affinity.getBoost(file), 5.0);
    }
}

QTEST_MAIN(TestTypeAffinity)
#include "test_type_affinity.moc"
