#include <QtTest/QtTest>

#define private public
#include "app/settings_controller.h"
#undef private

#include <QFile>
#include <QDir>
#include <QFileInfo>
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
};

void TestSettingsControllerPlatform::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
}

void TestSettingsControllerPlatform::cleanup()
{
    resetSettings();
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

QTEST_MAIN(TestSettingsControllerPlatform)
#include "test_settings_controller_platform.moc"
