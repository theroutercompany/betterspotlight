#include <QtTest/QtTest>

#define private public
#include "app/settings_controller.h"
#undef private

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>

#include <sqlite3.h>

namespace bs {

// Test-local stub so platform_integration.cpp can link in non-app test targets.
std::unique_ptr<PlatformIntegration> createApplePlatformIntegration()
{
    return nullptr;
}

} // namespace bs

namespace {

class MockPlatformIntegration final : public bs::PlatformIntegration {
public:
    bs::PlatformOperationResult launchResult{true, QStringLiteral("launch ok")};
    bs::PlatformOperationResult dockResult{true, QStringLiteral("dock ok")};
    int launchCalls = 0;
    int dockCalls = 0;

    bs::PlatformOperationResult setLaunchAtLogin(bool) override
    {
        ++launchCalls;
        return launchResult;
    }

    bs::PlatformOperationResult setShowInDock(bool) override
    {
        ++dockCalls;
        return dockResult;
    }
};

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/settings.json");
}

void resetSettings()
{
    QFile::remove(settingsPath());
}

void resetRuntimeDb()
{
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                  + QStringLiteral("/betterspotlight/index.db"));
}

QString runtimeDbPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/betterspotlight/index.db");
}

void ensureRuntimeSettingsTable()
{
    const QFileInfo dbInfo(runtimeDbPath());
    QDir().mkpath(dbInfo.absolutePath());
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(runtimeDbPath().toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return;
    }
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);",
                 nullptr,
                 nullptr,
                 nullptr);
    sqlite3_close(db);
}

void ensureFeedbackAndLearningTablesWithSeed()
{
    const QFileInfo dbInfo(runtimeDbPath());
    QDir().mkpath(dbInfo.absolutePath());
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(runtimeDbPath().toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return;
    }

    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS feedback (id INTEGER PRIMARY KEY);"
                 "CREATE TABLE IF NOT EXISTS interactions (id INTEGER PRIMARY KEY);"
                 "CREATE TABLE IF NOT EXISTS frequencies (id INTEGER PRIMARY KEY);"
                 "CREATE TABLE IF NOT EXISTS behavior_events_v1 (id INTEGER PRIMARY KEY);"
                 "CREATE TABLE IF NOT EXISTS training_examples_v1 (id INTEGER PRIMARY KEY);"
                 "CREATE TABLE IF NOT EXISTS replay_reservoir_v1 (slot INTEGER PRIMARY KEY);",
                 nullptr,
                 nullptr,
                 nullptr);
    sqlite3_exec(db,
                 "DELETE FROM feedback;"
                 "DELETE FROM interactions;"
                 "DELETE FROM frequencies;"
                 "DELETE FROM behavior_events_v1;"
                 "DELETE FROM training_examples_v1;"
                 "DELETE FROM replay_reservoir_v1;"
                 "INSERT INTO feedback (id) VALUES (1);"
                 "INSERT INTO interactions (id) VALUES (1);"
                 "INSERT INTO frequencies (id) VALUES (1);"
                 "INSERT INTO behavior_events_v1 (id) VALUES (1);"
                 "INSERT INTO training_examples_v1 (id) VALUES (1);"
                 "INSERT INTO replay_reservoir_v1 (slot) VALUES (1);",
                 nullptr,
                 nullptr,
                 nullptr);
    sqlite3_close(db);
}

int tableRowCount(const QString& tableName)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(runtimeDbPath().toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }

    sqlite3_stmt* stmt = nullptr;
    const QString sql = QStringLiteral("SELECT COUNT(*) FROM %1").arg(tableName);
    int rows = -1;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            rows = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

QJsonObject readSettings()
{
    QFile file(settingsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

} // namespace

class TestSettingsControllerPlatform : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void testLaunchAtLoginFailureDoesNotPersist();
    void testLaunchAtLoginSuccessPersists();
    void testShowInDockFailureDoesNotPersist();
    void testShowInDockSuccessPersists();
    void testRuntimeBoolSettingReadsDbValue();
    void testClearFeedbackDataPurgesLearningTables();
    void testExportDataIncludesLearningTables();
};

void TestSettingsControllerPlatform::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
    resetRuntimeDb();
}

void TestSettingsControllerPlatform::cleanup()
{
    resetSettings();
    resetRuntimeDb();
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                  + QStringLiteral("/betterspotlight-data-export.json"));
}

void TestSettingsControllerPlatform::testLaunchAtLoginFailureDoesNotPersist()
{
    bs::SettingsController controller;
    auto mock = std::make_unique<MockPlatformIntegration>();
    mock->launchResult = {false, QStringLiteral("launch failure")};
    MockPlatformIntegration* mockPtr = mock.get();
    controller.m_platformIntegration = std::move(mock);

    QSignalSpy changedSpy(&controller, &bs::SettingsController::launchAtLoginChanged);
    QVERIFY(!controller.launchAtLogin());

    controller.setLaunchAtLogin(true);

    QCOMPARE(mockPtr->launchCalls, 1);
    QCOMPARE(changedSpy.count(), 1);
    QVERIFY(!controller.launchAtLogin());
    QCOMPARE(controller.platformStatusKey(), QStringLiteral("launchAtLogin"));
    QVERIFY(!controller.platformStatusSuccess());
    QCOMPARE(controller.platformStatusMessage(), QStringLiteral("launch failure"));

    const QJsonObject settings = readSettings();
    QVERIFY(!settings.value(QStringLiteral("launchAtLogin")).toBool(false));
}

void TestSettingsControllerPlatform::testLaunchAtLoginSuccessPersists()
{
    bs::SettingsController controller;
    auto mock = std::make_unique<MockPlatformIntegration>();
    mock->launchResult = {true, QStringLiteral("launch enabled")};
    MockPlatformIntegration* mockPtr = mock.get();
    controller.m_platformIntegration = std::move(mock);

    QSignalSpy changedSpy(&controller, &bs::SettingsController::launchAtLoginChanged);
    QSignalSpy settingsSpy(&controller, &bs::SettingsController::settingsChanged);
    QVERIFY(!controller.launchAtLogin());

    controller.setLaunchAtLogin(true);

    QCOMPARE(mockPtr->launchCalls, 1);
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(settingsSpy.count(), 1);
    QVERIFY(controller.launchAtLogin());
    QCOMPARE(controller.platformStatusKey(), QStringLiteral("launchAtLogin"));
    QVERIFY(controller.platformStatusSuccess());
    QCOMPARE(controller.platformStatusMessage(), QStringLiteral("launch enabled"));

    const QJsonObject settings = readSettings();
    QVERIFY(settings.value(QStringLiteral("launchAtLogin")).toBool(false));
}

void TestSettingsControllerPlatform::testShowInDockFailureDoesNotPersist()
{
    bs::SettingsController controller;
    auto mock = std::make_unique<MockPlatformIntegration>();
    mock->dockResult = {false, QStringLiteral("dock failure")};
    MockPlatformIntegration* mockPtr = mock.get();
    controller.m_platformIntegration = std::move(mock);

    QSignalSpy changedSpy(&controller, &bs::SettingsController::showInDockChanged);
    QVERIFY(!controller.showInDock());

    controller.setShowInDock(true);

    QCOMPARE(mockPtr->dockCalls, 1);
    QCOMPARE(changedSpy.count(), 1);
    QVERIFY(!controller.showInDock());
    QCOMPARE(controller.platformStatusKey(), QStringLiteral("showInDock"));
    QVERIFY(!controller.platformStatusSuccess());
    QCOMPARE(controller.platformStatusMessage(), QStringLiteral("dock failure"));

    const QJsonObject settings = readSettings();
    QVERIFY(!settings.value(QStringLiteral("showInDock")).toBool(false));
}

void TestSettingsControllerPlatform::testShowInDockSuccessPersists()
{
    bs::SettingsController controller;
    auto mock = std::make_unique<MockPlatformIntegration>();
    mock->dockResult = {true, QStringLiteral("dock enabled")};
    MockPlatformIntegration* mockPtr = mock.get();
    controller.m_platformIntegration = std::move(mock);

    QSignalSpy changedSpy(&controller, &bs::SettingsController::showInDockChanged);
    QSignalSpy settingsSpy(&controller, &bs::SettingsController::settingsChanged);
    QVERIFY(!controller.showInDock());

    controller.setShowInDock(true);

    QCOMPARE(mockPtr->dockCalls, 1);
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(settingsSpy.count(), 1);
    QVERIFY(controller.showInDock());
    QCOMPARE(controller.platformStatusKey(), QStringLiteral("showInDock"));
    QVERIFY(controller.platformStatusSuccess());
    QCOMPARE(controller.platformStatusMessage(), QStringLiteral("dock enabled"));

    const QJsonObject settings = readSettings();
    QVERIFY(settings.value(QStringLiteral("showInDock")).toBool(false));
}

void TestSettingsControllerPlatform::testRuntimeBoolSettingReadsDbValue()
{
    bs::SettingsController controller;
    ensureRuntimeSettingsTable();

    QVERIFY(controller.setRuntimeSetting(QStringLiteral("behaviorStreamEnabled"),
                                         QStringLiteral("1")));
    QVERIFY(controller.runtimeBoolSetting(QStringLiteral("behaviorStreamEnabled"), false));

    QVERIFY(controller.setRuntimeSetting(QStringLiteral("behaviorStreamEnabled"),
                                         QStringLiteral("0")));
    QVERIFY(!controller.runtimeBoolSetting(QStringLiteral("behaviorStreamEnabled"), true));
}

void TestSettingsControllerPlatform::testClearFeedbackDataPurgesLearningTables()
{
    ensureFeedbackAndLearningTablesWithSeed();

    bs::SettingsController controller;
    controller.clearFeedbackData();

    QCOMPARE(tableRowCount(QStringLiteral("feedback")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("interactions")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("frequencies")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("behavior_events_v1")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("training_examples_v1")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("replay_reservoir_v1")), 0);
}

void TestSettingsControllerPlatform::testExportDataIncludesLearningTables()
{
    ensureFeedbackAndLearningTablesWithSeed();

    const QString downloadsDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QVERIFY2(!downloadsDir.isEmpty(), "Download location unavailable");
    QDir().mkpath(downloadsDir);
    const QString exportPath =
        downloadsDir + QStringLiteral("/betterspotlight-data-export.json");
    QFile::remove(exportPath);

    bs::SettingsController controller;
    controller.exportData();

    QFile file(exportPath);
    QVERIFY2(file.exists(), "Expected exported data file to exist");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, "Export JSON parse failed");
    QVERIFY(doc.isObject());

    const QJsonObject payload = doc.object();
    QVERIFY(payload.value(QStringLiteral("feedback")).isArray());
    QVERIFY(payload.value(QStringLiteral("interactions")).isArray());
    QVERIFY(payload.value(QStringLiteral("frequencies")).isArray());
    QVERIFY(payload.value(QStringLiteral("behaviorEvents")).isArray());
    QVERIFY(payload.value(QStringLiteral("trainingExamples")).isArray());
    QVERIFY(payload.value(QStringLiteral("replayReservoir")).isArray());

    QCOMPARE(payload.value(QStringLiteral("behaviorEvents")).toArray().size(), 1);
    QCOMPARE(payload.value(QStringLiteral("trainingExamples")).toArray().size(), 1);
    QCOMPARE(payload.value(QStringLiteral("replayReservoir")).toArray().size(), 1);
}

QTEST_MAIN(TestSettingsControllerPlatform)
#include "test_settings_controller_platform.moc"
