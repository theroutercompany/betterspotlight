#include <QtTest/QtTest>

#include "core/index/migration.h"

#include <sqlite3.h>

namespace {

QString settingValue(sqlite3* db, const char* key)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT value FROM settings WHERE key = ?1 LIMIT 1",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        return {};
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    QString value;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        value = QString::fromUtf8(raw ? raw : "");
    }
    sqlite3_finalize(stmt);
    return value;
}

} // namespace

class TestMigration : public QObject {
    Q_OBJECT

private slots:
    void testCurrentVersionMissingSettingsDefaultsToZero();
    void testCurrentVersionRejectsMalformedSetting();
    void testCurrentVersionRejectsNegativeSetting();
    void testCurrentVersionRejectsOverflowSetting();
    void testApplyMigrationsUpToV4();
    void testCurrentSchemaBackfillsLegacyInferenceBudgets();
    void testRejectsDowngrade();
    void testRejectsUnsupportedTargetVersionWithoutPartialMigration();
};

void TestMigration::testCurrentVersionMissingSettingsDefaultsToZero()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(bs::currentSchemaVersion(db), 0);

    sqlite3_close(db);
}

void TestMigration::testCurrentVersionRejectsMalformedSetting()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', 'not-a-version');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QCOMPARE(bs::currentSchemaVersion(db), -1);
    QVERIFY(!bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), -1);

    sqlite3_close(db);
}

void TestMigration::testCurrentVersionRejectsNegativeSetting()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '-1');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QCOMPARE(bs::currentSchemaVersion(db), -1);
    QVERIFY(!bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), -1);

    sqlite3_close(db);
}

void TestMigration::testCurrentVersionRejectsOverflowSetting()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES "
                          "('schema_version', '999999999999999999999999999999');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QCOMPARE(bs::currentSchemaVersion(db), -1);
    QVERIFY(!bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), -1);

    sqlite3_close(db);
}

void TestMigration::testApplyMigrationsUpToV4()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '1');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), 4);

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='vector_generation_state';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QVERIFY(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='behavior_events_v1';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QVERIFY(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerHealthWindowDays';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawHealthWindow = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawHealthWindow ? rawHealthWindow : ""), QStringLiteral("7"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerRecentCycleHistoryLimit';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawRecentCycleLimit = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawRecentCycleLimit ? rawRecentCycleLimit : ""), QStringLiteral("50"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerPromotionGateMinPositives';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawPromotionMinPos = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawPromotionMinPos ? rawPromotionMinPos : ""), QStringLiteral("80"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='behaviorCaptureAppActivityEnabled';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawCaptureApp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawCaptureApp ? rawCaptureApp : ""), QStringLiteral("1"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='behaviorCaptureInputActivityEnabled';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawCaptureInput = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawCaptureInput ? rawCaptureInput : ""), QStringLiteral("1"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='behaviorCaptureSearchEventsEnabled';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawCaptureSearch = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawCaptureSearch ? rawCaptureSearch : ""), QStringLiteral("1"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='behaviorCaptureWindowTitleHashEnabled';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawCaptureWindowTitle =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawCaptureWindowTitle ? rawCaptureWindowTitle : ""),
             QStringLiteral("1"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='behaviorCaptureBrowserHostHashEnabled';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawCaptureBrowserHost =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawCaptureBrowserHost ? rawCaptureBrowserHost : ""),
             QStringLiteral("1"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerNegativeSampleRatio';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawNegativeSampleRatio =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawNegativeSampleRatio ? rawNegativeSampleRatio : ""),
             QStringLiteral("3.0"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerMaxTrainingBatchSize';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawMaxTrainingBatch =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawMaxTrainingBatch ? rawMaxTrainingBatch : ""),
             QStringLiteral("1200"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerPromotionLatencyUsMax';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawLatencyUsMax =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawLatencyUsMax ? rawLatencyUsMax : ""),
             QStringLiteral("2500"));
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(
                 db,
                 "SELECT value FROM settings WHERE key='onlineRankerPromotionPredictionFailureRateMax';",
                 -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawFailureRateMax =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    QCOMPARE(QString::fromUtf8(rawFailureRateMax ? rawFailureRateMax : ""),
             QStringLiteral("0.05"));
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

void TestMigration::testCurrentSchemaBackfillsLegacyInferenceBudgets()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '4');"
                          "INSERT INTO settings (key, value) VALUES ('semanticBudgetMs', '70');"
                          "INSERT INTO settings (key, value) VALUES ('rerankBudgetMs', '120');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), 4);
    QCOMPARE(settingValue(db, "semanticBudgetMs"), QStringLiteral("350"));
    QCOMPARE(settingValue(db, "rerankBudgetMs"), QStringLiteral("600"));
    QCOMPARE(settingValue(db, "onlineRankerPromotionLatencyUsMax"), QStringLiteral("2500"));

    QCOMPARE(sqlite3_exec(db,
                          "UPDATE settings SET value = '225' WHERE key = 'semanticBudgetMs';"
                          "UPDATE settings SET value = '450' WHERE key = 'rerankBudgetMs';",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(bs::applyMigrations(db, 4));
    QCOMPARE(settingValue(db, "semanticBudgetMs"), QStringLiteral("225"));
    QCOMPARE(settingValue(db, "rerankBudgetMs"), QStringLiteral("450"));

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

    QVERIFY(!bs::applyMigrations(db, 4));
    QCOMPARE(bs::currentSchemaVersion(db), 5);

    sqlite3_close(db);
}

void TestMigration::testRejectsUnsupportedTargetVersionWithoutPartialMigration()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                          "INSERT INTO settings (key, value) VALUES ('schema_version', '1');",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(!bs::applyMigrations(db, 5));
    QCOMPARE(bs::currentSchemaVersion(db), 1);

    sqlite3_close(db);
}

QTEST_MAIN(TestMigration)
#include "test_migration.moc"
