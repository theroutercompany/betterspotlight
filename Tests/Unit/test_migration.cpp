#include <QtTest/QtTest>

#include "core/index/migration.h"

#include <sqlite3.h>

class TestMigration : public QObject {
    Q_OBJECT

private slots:
    void testCurrentVersionMissingSettingsDefaultsToZero();
    void testApplyMigrationsUpToV3();
    void testRejectsDowngrade();
    void testRejectsUnsupportedTargetVersion();
};

void TestMigration::testCurrentVersionMissingSettingsDefaultsToZero()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(bs::currentSchemaVersion(db), 0);

    sqlite3_close(db);
}

void TestMigration::testApplyMigrationsUpToV3()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '1');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(bs::applyMigrations(db, 3));
    QCOMPARE(bs::currentSchemaVersion(db), 3);

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='vector_generation_state';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QVERIFY(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

void TestMigration::testRejectsDowngrade()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '5');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(!bs::applyMigrations(db, 3));
    QCOMPARE(bs::currentSchemaVersion(db), 5);

    sqlite3_close(db);
}

void TestMigration::testRejectsUnsupportedTargetVersion()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '1');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(!bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), 3);

    sqlite3_close(db);
}

QTEST_MAIN(TestMigration)
#include "test_migration.moc"

