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
#include <QTemporaryDir>

#include <sqlite3.h>

#include <cmath>
#include <limits>

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
    const QString configuredSettingsDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_SETTINGS_DIR").trimmed();
    const QString dataDir = configuredSettingsDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : configuredSettingsDir;
    return QDir(dataDir).filePath(QStringLiteral("settings.json"));
}

QString runtimeDataDir()
{
    const QString configuredDataDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_DATA_DIR").trimmed();
    if (!configuredDataDir.isEmpty()) {
        return configuredDataDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/betterspotlight");
}

void resetSettings()
{
    QFile::remove(settingsPath());
}

void resetRuntimeDb()
{
    QDir(runtimeDataDir()).removeRecursively();
}

QString runtimeDbPath()
{
    return QDir(runtimeDataDir()).filePath(QStringLiteral("index.db"));
}

QString exportDir()
{
    const QString configuredExportDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_EXPORT_DIR").trimmed();
    if (!configuredExportDir.isEmpty()) {
        return configuredExportDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

bool runtimeSettingValue(const QString& key, QString* value)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(runtimeDbPath().toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    bool found = false;
    static constexpr const char* kSql =
        "SELECT value FROM settings WHERE key = ?1 LIMIT 1";
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK) {
        const QByteArray keyUtf8 = key.toUtf8();
        sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (value) {
                value->clear();
                const unsigned char* raw = sqlite3_column_text(stmt, 0);
                if (raw) {
                    *value = QString::fromUtf8(reinterpret_cast<const char*>(raw));
                }
            }
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
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

bool writeSettings(const QJsonObject& settings)
{
    const QString path = settingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented)) > 0;
}

} // namespace

class TestSettingsControllerPlatform : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testLaunchAtLoginFailureDoesNotPersist();
    void testLaunchAtLoginSuccessPersists();
    void testShowInDockFailureDoesNotPersist();
    void testShowInDockSuccessPersists();
    void testIndexRootsSanitizeInvalidEntriesAndPreserveSkipMode();
    void testIndexRootModeNormalizationAcceptsLegacyAndCasedInput();
    void testNonFiniteDoubleSettingsAreClampedBeforePersisting();
    void testLoadedSettingsNormalizeTypedDriftForUiAndRuntimeDb();
    void testAutoVectorMigrationRoundTripsAsGuidanceSetting();
    void testRuntimeBoolSettingReadsDbValue();
    void testRuntimeSettingCreatesTableForMissingDb();
    void testRuntimeSettingDoesNotSignalOnOpenFailure();
    void testClearFeedbackDataPurgesLearningTables();
    void testClearFeedbackDataDoesNotSignalOnDeleteFailure();
    void testExportDataIncludesLearningTables();
    void testExportDataReportsWriteFailure();

private:
    QTemporaryDir m_tempHome;
    QTemporaryDir m_tempSettingsDir;
    QTemporaryDir m_tempDataDir;
    QTemporaryDir m_tempExportDir;
    QByteArray m_oldHome;
    QByteArray m_oldSettingsDir;
    QByteArray m_oldDataDir;
    QByteArray m_oldExportDir;
    bool m_hadHome = false;
    bool m_hadSettingsDir = false;
    bool m_hadDataDir = false;
    bool m_hadExportDir = false;
};

void TestSettingsControllerPlatform::initTestCase()
{
    QVERIFY(m_tempHome.isValid());
    QVERIFY(m_tempSettingsDir.isValid());
    QVERIFY(m_tempDataDir.isValid());
    QVERIFY(m_tempExportDir.isValid());

    m_hadHome = qEnvironmentVariableIsSet("HOME");
    m_oldHome = qgetenv("HOME");
    m_hadSettingsDir = qEnvironmentVariableIsSet("BETTERSPOTLIGHT_SETTINGS_DIR");
    m_oldSettingsDir = qgetenv("BETTERSPOTLIGHT_SETTINGS_DIR");
    m_hadDataDir = qEnvironmentVariableIsSet("BETTERSPOTLIGHT_DATA_DIR");
    m_oldDataDir = qgetenv("BETTERSPOTLIGHT_DATA_DIR");
    m_hadExportDir = qEnvironmentVariableIsSet("BETTERSPOTLIGHT_EXPORT_DIR");
    m_oldExportDir = qgetenv("BETTERSPOTLIGHT_EXPORT_DIR");

    QCoreApplication::setOrganizationName(QStringLiteral("BetterSpotlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test-settings-controller-platform"));
    qputenv("HOME", m_tempHome.path().toUtf8());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", m_tempSettingsDir.path().toUtf8());
    qputenv("BETTERSPOTLIGHT_DATA_DIR", m_tempDataDir.path().toUtf8());
    qputenv("BETTERSPOTLIGHT_EXPORT_DIR", m_tempExportDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
    resetRuntimeDb();
}

void TestSettingsControllerPlatform::cleanupTestCase()
{
    QStandardPaths::setTestModeEnabled(false);

    if (m_hadHome) {
        qputenv("HOME", m_oldHome);
    } else {
        qunsetenv("HOME");
    }

    if (m_hadSettingsDir) {
        qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", m_oldSettingsDir);
    } else {
        qunsetenv("BETTERSPOTLIGHT_SETTINGS_DIR");
    }

    if (m_hadDataDir) {
        qputenv("BETTERSPOTLIGHT_DATA_DIR", m_oldDataDir);
    } else {
        qunsetenv("BETTERSPOTLIGHT_DATA_DIR");
    }

    if (m_hadExportDir) {
        qputenv("BETTERSPOTLIGHT_EXPORT_DIR", m_oldExportDir);
    } else {
        qunsetenv("BETTERSPOTLIGHT_EXPORT_DIR");
    }
}

void TestSettingsControllerPlatform::cleanup()
{
    resetSettings();
    resetRuntimeDb();
    const QString exportPath =
        QDir(exportDir()).filePath(QStringLiteral("betterspotlight-data-export.json"));
    if (QFileInfo(exportPath).isDir()) {
        QDir(exportPath).removeRecursively();
    } else {
        QFile::remove(exportPath);
    }
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

void TestSettingsControllerPlatform::testIndexRootsSanitizeInvalidEntriesAndPreserveSkipMode()
{
    bs::SettingsController controller;
    QSignalSpy rootsSpy(&controller, &bs::SettingsController::indexRootsChanged);
    QSignalSpy settingsSpy(&controller, &bs::SettingsController::settingsChanged);

    const QString skippedRoot = QDir(m_tempHome.path()).filePath(QStringLiteral("Skip Me"));
    const QString urlRoot = QDir(m_tempHome.path()).filePath(QStringLiteral("URL Root"));

    QVariantList roots;
    roots.append(QVariantMap{
        {QStringLiteral("path"), skippedRoot},
        {QStringLiteral("mode"), QStringLiteral("skip")},
    });
    roots.append(QVariantMap{
        {QStringLiteral("path"), QStringLiteral("relative/path")},
        {QStringLiteral("mode"), QStringLiteral("index_embed")},
    });
    roots.append(QVariantMap{
        {QStringLiteral("path"), QStringLiteral("   ")},
        {QStringLiteral("mode"), QStringLiteral("index_embed")},
    });
    roots.append(QVariantMap{
        {QStringLiteral("path"), skippedRoot},
        {QStringLiteral("mode"), QStringLiteral("index_only")},
    });
    roots.append(QVariantMap{
        {QStringLiteral("path"), QStringLiteral("file://") + urlRoot},
        {QStringLiteral("mode"), QStringLiteral("index_only")},
    });
    roots.append(QStringLiteral("not-a-map"));

    controller.setIndexRoots(roots);

    QCOMPARE(rootsSpy.count(), 1);
    QCOMPARE(settingsSpy.count(), 1);

    const QVariantList normalized = controller.indexRoots();
    QCOMPARE(normalized.size(), 2);

    const QVariantMap first = normalized.at(0).toMap();
    QCOMPARE(first.value(QStringLiteral("path")).toString(), QDir::cleanPath(skippedRoot));
    QCOMPARE(first.value(QStringLiteral("mode")).toString(), QStringLiteral("skip"));

    const QVariantMap second = normalized.at(1).toMap();
    QCOMPARE(second.value(QStringLiteral("path")).toString(), QDir::cleanPath(urlRoot));
    QCOMPARE(second.value(QStringLiteral("mode")).toString(), QStringLiteral("index_only"));

    const QJsonArray persisted = readSettings().value(QStringLiteral("indexRoots")).toArray();
    QCOMPARE(persisted.size(), 2);
    QCOMPARE(persisted.at(0).toObject().value(QStringLiteral("mode")).toString(),
             QStringLiteral("skip"));
    QCOMPARE(persisted.at(1).toObject().value(QStringLiteral("path")).toString(),
             QDir::cleanPath(urlRoot));
}

void TestSettingsControllerPlatform::testIndexRootModeNormalizationAcceptsLegacyAndCasedInput()
{
    bs::SettingsController controller;

    const QString legacyRoot = QDir(m_tempHome.path()).filePath(QStringLiteral("Legacy"));
    const QString casedRoot = QDir(m_tempHome.path()).filePath(QStringLiteral("Cased"));
    const QString missingModeRoot = QDir(m_tempHome.path()).filePath(QStringLiteral("Missing"));

    QVariantList roots;
    roots.append(QVariantMap{
        {QStringLiteral("path"), legacyRoot},
        {QStringLiteral("mode"), QStringLiteral("index")},
    });
    roots.append(QVariantMap{
        {QStringLiteral("path"), casedRoot},
        {QStringLiteral("mode"), QStringLiteral(" INDEX_EMBED ")},
    });
    roots.append(QVariantMap{
        {QStringLiteral("path"), missingModeRoot},
    });

    controller.setIndexRoots(roots);

    const QVariantList normalized = controller.indexRoots();
    QCOMPARE(normalized.size(), 3);
    QCOMPARE(normalized.at(0).toMap().value(QStringLiteral("mode")).toString(),
             QStringLiteral("index_only"));
    QCOMPARE(normalized.at(1).toMap().value(QStringLiteral("mode")).toString(),
             QStringLiteral("index_embed"));
    QCOMPARE(normalized.at(2).toMap().value(QStringLiteral("mode")).toString(),
             QStringLiteral("index_embed"));

    const QJsonArray persisted = readSettings().value(QStringLiteral("indexRoots")).toArray();
    QCOMPARE(persisted.at(0).toObject().value(QStringLiteral("mode")).toString(),
             QStringLiteral("index_only"));
    QCOMPARE(persisted.at(1).toObject().value(QStringLiteral("mode")).toString(),
             QStringLiteral("index_embed"));
    QCOMPARE(persisted.at(2).toObject().value(QStringLiteral("mode")).toString(),
             QStringLiteral("index_embed"));
}

void TestSettingsControllerPlatform::testNonFiniteDoubleSettingsAreClampedBeforePersisting()
{
    bs::SettingsController controller;

    controller.setQueryRouterMinConfidence(0.9);
    QCOMPARE(controller.queryRouterMinConfidence(), 0.9);
    controller.setQueryRouterMinConfidence(std::numeric_limits<double>::quiet_NaN());
    QCOMPARE(controller.queryRouterMinConfidence(), 0.45);

    controller.setBm25WeightName(2.5);
    QCOMPARE(controller.bm25WeightName(), 2.5);
    controller.setBm25WeightName(std::numeric_limits<double>::infinity());
    QCOMPARE(controller.bm25WeightName(), 10.0);

    controller.setBm25WeightPath(-7.0);
    QCOMPARE(controller.bm25WeightPath(), 0.0);

    controller.setBm25WeightContent(4.0);
    QCOMPARE(controller.bm25WeightContent(), 4.0);
    controller.setBm25WeightContent(-std::numeric_limits<double>::infinity());
    QCOMPARE(controller.bm25WeightContent(), 1.0);

    const QJsonObject settings = readSettings();
    QVERIFY(std::isfinite(settings.value(QStringLiteral("queryRouterMinConfidence")).toDouble()));
    QVERIFY(std::isfinite(settings.value(QStringLiteral("bm25WeightName")).toDouble()));
    QVERIFY(std::isfinite(settings.value(QStringLiteral("bm25WeightPath")).toDouble()));
    QVERIFY(std::isfinite(settings.value(QStringLiteral("bm25WeightContent")).toDouble()));

    QString storedValue;
    QVERIFY(runtimeSettingValue(QStringLiteral("queryRouterMinConfidence"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("0.45"));
    QVERIFY(runtimeSettingValue(QStringLiteral("bm25WeightName"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("10"));
    QVERIFY(runtimeSettingValue(QStringLiteral("bm25WeightPath"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("0"));
    QVERIFY(runtimeSettingValue(QStringLiteral("bm25WeightContent"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1"));
}

void TestSettingsControllerPlatform::testLoadedSettingsNormalizeTypedDriftForUiAndRuntimeDb()
{
    QJsonArray malformedRoots;
    malformedRoots.append(QStringLiteral("not-a-root-object"));

    QJsonObject raw;
    raw[QStringLiteral("indexRoots")] = malformedRoots;
    raw[QStringLiteral("maxResults")] = -100;
    raw[QStringLiteral("embeddingEnabled")] = QStringLiteral("off");
    raw[QStringLiteral("inferenceServiceEnabled")] = QStringLiteral("yes");
    raw[QStringLiteral("queryRouterEnabled")] = QStringLiteral("0");
    raw[QStringLiteral("queryRouterMinConfidence")] = QStringLiteral("2.5");
    raw[QStringLiteral("strongEmbeddingTopK")] = 500;
    raw[QStringLiteral("fastEmbeddingTopK")] = QStringLiteral("400");
    raw[QStringLiteral("rerankerStage1Max")] = 0;
    raw[QStringLiteral("rerankerStage2Max")] = 999;
    raw[QStringLiteral("semanticBudgetMs")] = 5;
    raw[QStringLiteral("rerankBudgetMs")] = QStringLiteral("900");
    raw[QStringLiteral("maxFileSizeMB")] = 0;
    raw[QStringLiteral("extractionTimeoutMs")] = 999999;
    raw[QStringLiteral("feedbackRetentionDays")] = 1;
    raw[QStringLiteral("bm25WeightName")] = -2.5;
    raw[QStringLiteral("bm25WeightPath")] = QStringLiteral("nan");
    raw[QStringLiteral("bm25WeightContent")] = QStringLiteral("3.25");
    raw[QStringLiteral("qaSnippetEnabled")] = QStringLiteral("off");
    raw[QStringLiteral("enableInteractionTracking")] = QStringLiteral("on");
    raw[QStringLiteral("behaviorStreamEnabled")] = QStringLiteral("on");
    raw[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("definitely_invalid");
    raw[QStringLiteral("onlineRankerBlendAlpha")] = 2.0;
    raw[QStringLiteral("behaviorRawRetentionDays")] = -5;

    QVERIFY(writeSettings(raw));

    bs::SettingsController controller;

    QCOMPARE(controller.indexRoots().size(), 3);
    QCOMPARE(controller.maxResults(), 5);
    QVERIFY(!controller.embeddingEnabled());
    QVERIFY(controller.inferenceServiceEnabled());
    QVERIFY(!controller.queryRouterEnabled());
    QCOMPARE(controller.queryRouterMinConfidence(), 1.0);
    QCOMPARE(controller.strongEmbeddingTopK(), 200);
    QCOMPARE(controller.fastEmbeddingTopK(), 300);
    QCOMPARE(controller.rerankerStage1Max(), 4);
    QCOMPARE(controller.rerankerStage2Max(), 100);
    QCOMPARE(controller.semanticBudgetMs(), 20);
    QCOMPARE(controller.rerankBudgetMs(), 600);
    QCOMPARE(controller.maxFileSizeMB(), 1);
    QCOMPARE(controller.extractionTimeoutMs(), 120000);
    QCOMPARE(controller.feedbackRetentionDays(), 7);
    QCOMPARE(controller.bm25WeightName(), 0.0);
    QCOMPARE(controller.bm25WeightPath(), 5.0);
    QCOMPARE(controller.bm25WeightContent(), 3.25);
    QVERIFY(!controller.qaSnippetEnabled());
    QVERIFY(controller.enableInteractionTracking());
    QVERIFY(controller.runtimeBoolSetting(QStringLiteral("behaviorStreamEnabled"), false));

    const QJsonObject persisted = readSettings();
    QCOMPARE(persisted.value(QStringLiteral("indexRoots")).toArray().size(), 3);
    QCOMPARE(persisted.value(QStringLiteral("maxResults")).toInt(), 5);
    QVERIFY(persisted.value(QStringLiteral("embeddingEnabled")).isBool());
    QVERIFY(!persisted.value(QStringLiteral("embeddingEnabled")).toBool(true));
    QCOMPARE(persisted.value(QStringLiteral("strongEmbeddingTopK")).toInt(), 200);
    QCOMPARE(persisted.value(QStringLiteral("fastEmbeddingTopK")).toInt(), 300);
    QCOMPARE(persisted.value(QStringLiteral("semanticBudgetMs")).toInt(), 20);
    QCOMPARE(persisted.value(QStringLiteral("rerankBudgetMs")).toInt(), 600);
    QCOMPARE(persisted.value(QStringLiteral("feedbackRetentionDays")).toInt(), 7);
    QCOMPARE(persisted.value(QStringLiteral("onlineRankerRolloutMode")).toString(),
             QStringLiteral("instrumentation_only"));

    QString storedValue;
    QVERIFY(runtimeSettingValue(QStringLiteral("embeddingEnabled"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("0"));
    QVERIFY(runtimeSettingValue(QStringLiteral("inferenceServiceEnabled"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1"));
    QVERIFY(runtimeSettingValue(QStringLiteral("queryRouterEnabled"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("0"));
    QVERIFY(runtimeSettingValue(QStringLiteral("queryRouterMinConfidence"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1.00"));
    QVERIFY(runtimeSettingValue(QStringLiteral("strongEmbeddingTopK"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("200"));
    QVERIFY(runtimeSettingValue(QStringLiteral("fastEmbeddingTopK"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("300"));
    QVERIFY(runtimeSettingValue(QStringLiteral("semanticBudgetMs"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("20"));
    QVERIFY(runtimeSettingValue(QStringLiteral("rerankBudgetMs"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("600"));
    QVERIFY(runtimeSettingValue(QStringLiteral("max_file_size"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1048576"));
    QVERIFY(runtimeSettingValue(QStringLiteral("extraction_timeout_ms"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("120000"));
    QVERIFY(runtimeSettingValue(QStringLiteral("bm25WeightName"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("0"));
    QVERIFY(runtimeSettingValue(QStringLiteral("bm25WeightPath"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("5"));
    QVERIFY(runtimeSettingValue(QStringLiteral("bm25WeightContent"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("3.25"));
    QVERIFY(runtimeSettingValue(QStringLiteral("onlineRankerRolloutMode"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("instrumentation_only"));
    QVERIFY(runtimeSettingValue(QStringLiteral("onlineRankerBlendAlpha"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1"));
    QVERIFY(runtimeSettingValue(QStringLiteral("behaviorRawRetentionDays"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1"));
}

void TestSettingsControllerPlatform::testAutoVectorMigrationRoundTripsAsGuidanceSetting()
{
    bs::SettingsController controller;
    QSignalSpy changedSpy(&controller, &bs::SettingsController::autoVectorMigrationChanged);
    QSignalSpy settingsSpy(&controller, &bs::SettingsController::settingsChanged);

    QVERIFY(controller.autoVectorMigration());
    controller.setAutoVectorMigration(false);

    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(settingsSpy.count(), 1);
    QVERIFY(!controller.autoVectorMigration());

    const QJsonObject settings = readSettings();
    QVERIFY(settings.contains(QStringLiteral("autoVectorMigration")));
    QVERIFY(!settings.value(QStringLiteral("autoVectorMigration")).toBool(true));
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

void TestSettingsControllerPlatform::testRuntimeSettingCreatesTableForMissingDb()
{
    bs::SettingsController controller;
    resetRuntimeDb();

    QSignalSpy settingsSpy(&controller, &bs::SettingsController::settingsChanged);

    QVERIFY(controller.setRuntimeSetting(QStringLiteral("behaviorStreamEnabled"),
                                         QStringLiteral("1")));
    QCOMPARE(settingsSpy.count(), 1);

    QString storedValue;
    QVERIFY(runtimeSettingValue(QStringLiteral("behaviorStreamEnabled"), &storedValue));
    QCOMPARE(storedValue, QStringLiteral("1"));
    QVERIFY(controller.runtimeBoolSetting(QStringLiteral("behaviorStreamEnabled"), false));
    QVERIFY(QFileInfo(runtimeDbPath()).isFile());
}

void TestSettingsControllerPlatform::testRuntimeSettingDoesNotSignalOnOpenFailure()
{
    bs::SettingsController controller;
    resetRuntimeDb();
    QVERIFY(QDir().mkpath(runtimeDbPath()));

    QSignalSpy settingsSpy(&controller, &bs::SettingsController::settingsChanged);

    QVERIFY(!controller.setRuntimeSetting(QStringLiteral("behaviorStreamEnabled"),
                                          QStringLiteral("1")));
    QCOMPARE(settingsSpy.count(), 0);
}

void TestSettingsControllerPlatform::testClearFeedbackDataPurgesLearningTables()
{
    ensureFeedbackAndLearningTablesWithSeed();

    bs::SettingsController controller;
    QSignalSpy clearedSpy(&controller, &bs::SettingsController::feedbackDataCleared);
    QVERIFY(controller.clearFeedbackData());
    QCOMPARE(clearedSpy.count(), 1);

    QCOMPARE(tableRowCount(QStringLiteral("feedback")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("interactions")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("frequencies")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("behavior_events_v1")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("training_examples_v1")), 0);
    QCOMPARE(tableRowCount(QStringLiteral("replay_reservoir_v1")), 0);
}

void TestSettingsControllerPlatform::testClearFeedbackDataDoesNotSignalOnDeleteFailure()
{
    const QFileInfo dbInfo(runtimeDbPath());
    QDir().mkpath(dbInfo.absolutePath());
    sqlite3* db = nullptr;
    QVERIFY(sqlite3_open_v2(runtimeDbPath().toUtf8().constData(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
            == SQLITE_OK);
    QVERIFY(sqlite3_exec(db,
                         "CREATE TABLE feedback (id INTEGER PRIMARY KEY);"
                         "INSERT INTO feedback (id) VALUES (1);",
                         nullptr,
                         nullptr,
                         nullptr)
            == SQLITE_OK);
    sqlite3_close(db);

    bs::SettingsController controller;
    QSignalSpy clearedSpy(&controller, &bs::SettingsController::feedbackDataCleared);

    QVERIFY(!controller.clearFeedbackData());
    QCOMPARE(clearedSpy.count(), 0);
    QVERIFY(!controller.platformStatusSuccess());
    QCOMPARE(tableRowCount(QStringLiteral("feedback")), 1);
}

void TestSettingsControllerPlatform::testExportDataIncludesLearningTables()
{
    ensureFeedbackAndLearningTablesWithSeed();

    const QString downloadsDir = exportDir();
    QVERIFY2(!downloadsDir.isEmpty(), "Download location unavailable");
    QDir().mkpath(downloadsDir);
    const QString exportPath =
        downloadsDir + QStringLiteral("/betterspotlight-data-export.json");
    QFile::remove(exportPath);

    bs::SettingsController controller;
    QVERIFY(controller.exportData());

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

void TestSettingsControllerPlatform::testExportDataReportsWriteFailure()
{
    ensureFeedbackAndLearningTablesWithSeed();

    const QString downloadsDir = exportDir();
    QVERIFY2(!downloadsDir.isEmpty(), "Download location unavailable");
    QDir().mkpath(downloadsDir);
    const QString exportPath =
        downloadsDir + QStringLiteral("/betterspotlight-data-export.json");
    QFile::remove(exportPath);
    QVERIFY(QDir().mkpath(exportPath));

    bs::SettingsController controller;

    QVERIFY(!controller.exportData());
    QVERIFY(!controller.platformStatusSuccess());
    QCOMPARE(controller.platformStatusKey(), QStringLiteral("exportData"));
}

QTEST_MAIN(TestSettingsControllerPlatform)
#include "test_settings_controller_platform.moc"
