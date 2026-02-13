#include "settings_controller.h"
#include "platform_integration.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>

namespace bs {

namespace {

QJsonArray defaultIndexRoots()
{
    const QString home = QDir::homePath();

    QJsonArray roots;
    roots.append(QJsonObject{
        {QStringLiteral("path"), home + QStringLiteral("/Documents")},
        {QStringLiteral("mode"), QStringLiteral("index_embed")},
    });
    roots.append(QJsonObject{
        {QStringLiteral("path"), home + QStringLiteral("/Desktop")},
        {QStringLiteral("mode"), QStringLiteral("index_embed")},
    });
    roots.append(QJsonObject{
        {QStringLiteral("path"), home + QStringLiteral("/Downloads")},
        {QStringLiteral("mode"), QStringLiteral("index_embed")},
    });
    return roots;
}

QJsonArray indexRootsFromHomeDirectories(const QJsonObject& settings)
{
    QJsonArray roots;
    const QString home = QDir::homePath();
    const QJsonArray homeDirectories =
        settings.value(QStringLiteral("home_directories")).toArray();
    for (const QJsonValue& value : homeDirectories) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        const QString mode = obj.value(QStringLiteral("mode")).toString();
        if (mode == QLatin1String("skip")) {
            continue;
        }
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        roots.append(QJsonObject{
            {QStringLiteral("path"), home + QLatin1Char('/') + name},
            {QStringLiteral("mode"), mode.isEmpty() ? QStringLiteral("index_only") : mode},
        });
    }
    return roots;
}

QStringList jsonArrayToStringList(const QJsonArray& arr)
{
    QStringList out;
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        out.append(v.toString());
    }
    return out;
}

QJsonArray stringListToJsonArray(const QStringList& values)
{
    QJsonArray out;
    for (const QString& v : values) {
        out.append(v);
    }
    return out;
}

QVariantList jsonArrayToVariantList(const QJsonArray& arr)
{
    QVariantList out;
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        out.append(v.toObject().toVariantMap());
    }
    return out;
}

QJsonArray variantListToJsonArray(const QVariantList& values)
{
    QJsonArray out;

    for (const QVariant& v : values) {
        const QVariantMap map = v.toMap();
        QJsonObject obj;
        obj[QStringLiteral("path")] = map.value(QStringLiteral("path")).toString();
        obj[QStringLiteral("mode")] = map.value(QStringLiteral("mode"), QStringLiteral("index_embed")).toString();
        out.append(obj);
    }

    return out;
}

void ensureDefault(QJsonObject& obj, const QString& key, const QJsonValue& value)
{
    if (!obj.contains(key)) {
        obj.insert(key, value);
    }
}

void upsertSetting(sqlite3* db, const QString& key, const QString& value)
{
    if (!db) {
        return;
    }
    static constexpr const char* kSql = R"(
        INSERT INTO settings (key, value) VALUES (?1, ?2)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, valueUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

QString boolToSqlValue(bool value)
{
    return value ? QStringLiteral("1") : QStringLiteral("0");
}

bool parseBoolValue(const QString& raw, bool defaultValue)
{
    const QString normalized = raw.trimmed().toLower();
    if (normalized.isEmpty()) {
        return defaultValue;
    }
    if (normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on")) {
        return true;
    }
    if (normalized == QLatin1String("0")
        || normalized == QLatin1String("false")
        || normalized == QLatin1String("no")
        || normalized == QLatin1String("off")) {
        return false;
    }
    return defaultValue;
}

bool jsonBoolValue(const QJsonValue& value, bool defaultValue)
{
    if (value.isBool()) {
        return value.toBool(defaultValue);
    }
    if (value.isDouble()) {
        return std::abs(value.toDouble(0.0)) > 1e-9;
    }
    if (value.isString()) {
        return parseBoolValue(value.toString(), defaultValue);
    }
    return defaultValue;
}

void syncRuntimeSettingsToDb(const QJsonObject& settings)
{
    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/betterspotlight/index.db");
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return;
    }

    upsertSetting(db, QStringLiteral("embeddingEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("embeddingEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("inferenceServiceEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("inferenceServiceEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("inferenceEmbedOffloadEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("inferenceEmbedOffloadEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("inferenceRerankOffloadEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("inferenceRerankOffloadEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("inferenceQaOffloadEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("inferenceQaOffloadEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("inferenceShadowModeEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("inferenceShadowModeEnabled")).toBool(false)));
    upsertSetting(db, QStringLiteral("queryRouterEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("queryRouterEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("queryRouterMinConfidence"),
                  QString::number(settings.value(QStringLiteral("queryRouterMinConfidence")).toDouble(0.45), 'f', 2));
    upsertSetting(db, QStringLiteral("fastEmbeddingEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("fastEmbeddingEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("dualEmbeddingFusionEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("dualEmbeddingFusionEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("strongEmbeddingTopK"),
                  QString::number(settings.value(QStringLiteral("strongEmbeddingTopK")).toInt(40)));
    upsertSetting(db, QStringLiteral("fastEmbeddingTopK"),
                  QString::number(settings.value(QStringLiteral("fastEmbeddingTopK")).toInt(60)));
    upsertSetting(db, QStringLiteral("rerankerCascadeEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("rerankerCascadeEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("rerankerStage1Max"),
                  QString::number(settings.value(QStringLiteral("rerankerStage1Max")).toInt(40)));
    upsertSetting(db, QStringLiteral("rerankerStage2Max"),
                  QString::number(settings.value(QStringLiteral("rerankerStage2Max")).toInt(12)));
    upsertSetting(db, QStringLiteral("autoVectorMigration"),
                  boolToSqlValue(settings.value(QStringLiteral("autoVectorMigration")).toBool(true)));
    upsertSetting(db, QStringLiteral("bm25WeightName"),
                  QString::number(settings.value(QStringLiteral("bm25WeightName")).toDouble(10.0), 'g', 17));
    upsertSetting(db, QStringLiteral("bm25WeightPath"),
                  QString::number(settings.value(QStringLiteral("bm25WeightPath")).toDouble(5.0), 'g', 17));
    upsertSetting(db, QStringLiteral("bm25WeightContent"),
                  QString::number(settings.value(QStringLiteral("bm25WeightContent")).toDouble(1.0), 'g', 17));
    upsertSetting(db, QStringLiteral("qaSnippetEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("qaSnippetEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("personalizedLtrEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("personalizedLtrEnabled")).toBool(true)));
    upsertSetting(db, QStringLiteral("behaviorStreamEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("behaviorStreamEnabled")).toBool(false)));
    upsertSetting(db, QStringLiteral("learningEnabled"),
                  boolToSqlValue(settings.value(QStringLiteral("learningEnabled")).toBool(false)));
    upsertSetting(db, QStringLiteral("learningPauseOnUserInput"),
                  boolToSqlValue(settings.value(QStringLiteral("learningPauseOnUserInput")).toBool(true)));
    upsertSetting(db, QStringLiteral("onlineRankerBlendAlpha"),
                  QString::number(settings.value(QStringLiteral("onlineRankerBlendAlpha")).toDouble(0.15), 'g', 17));
    upsertSetting(db, QStringLiteral("behaviorRawRetentionDays"),
                  QString::number(settings.value(QStringLiteral("behaviorRawRetentionDays")).toInt(30)));
    upsertSetting(db, QStringLiteral("semanticBudgetMs"),
                  QString::number(settings.value(QStringLiteral("semanticBudgetMs")).toInt(70)));
    upsertSetting(db, QStringLiteral("rerankBudgetMs"),
                  QString::number(settings.value(QStringLiteral("rerankBudgetMs")).toInt(120)));
    upsertSetting(db, QStringLiteral("max_file_size"),
                  QString::number(settings.value(QStringLiteral("maxFileSizeMB")).toInt(50) * 1024 * 1024));
    upsertSetting(db, QStringLiteral("extraction_timeout_ms"),
                  QString::number(settings.value(QStringLiteral("extractionTimeoutMs")).toInt(30000)));
    sqlite3_close(db);
}

} // namespace

SettingsController::SettingsController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    m_platformIntegration = PlatformIntegration::create();

    if (showInDock()) {
        // Defer Dock policy mutation until the app event loop starts; on macOS,
        // early activation-policy calls can be ignored during app bootstrap.
        QTimer::singleShot(0, this, [this]() {
            const PlatformOperationResult result = m_platformIntegration->setShowInDock(true);
            if (!result.success) {
                setPlatformStatus(QStringLiteral("showInDock"), false, result.message);
            }
        });
    }
}

QString SettingsController::hotkey() const
{
    return m_settings.value(QStringLiteral("hotkey")).toString(QStringLiteral("Cmd+Space"));
}

bool SettingsController::launchAtLogin() const
{
    return m_settings.value(QStringLiteral("launchAtLogin")).toBool(false);
}

bool SettingsController::showInDock() const
{
    return m_settings.value(QStringLiteral("showInDock")).toBool(false);
}

bool SettingsController::checkForUpdates() const
{
    return m_settings.value(QStringLiteral("checkForUpdates")).toBool(true);
}

int SettingsController::maxResults() const
{
    return m_settings.value(QStringLiteral("maxResults")).toInt(20);
}

QVariantList SettingsController::indexRoots() const
{
    return jsonArrayToVariantList(m_settings.value(QStringLiteral("indexRoots")).toArray());
}

bool SettingsController::enablePdf() const
{
    return m_settings.value(QStringLiteral("enablePdf")).toBool(true);
}

bool SettingsController::enableOcr() const
{
    return m_settings.value(QStringLiteral("enableOcr")).toBool(false);
}

bool SettingsController::embeddingEnabled() const
{
    return m_settings.value(QStringLiteral("embeddingEnabled")).toBool(false);
}

bool SettingsController::inferenceServiceEnabled() const
{
    return m_settings.value(QStringLiteral("inferenceServiceEnabled")).toBool(true);
}

bool SettingsController::inferenceEmbedOffloadEnabled() const
{
    return m_settings.value(QStringLiteral("inferenceEmbedOffloadEnabled")).toBool(true);
}

bool SettingsController::inferenceRerankOffloadEnabled() const
{
    return m_settings.value(QStringLiteral("inferenceRerankOffloadEnabled")).toBool(true);
}

bool SettingsController::inferenceQaOffloadEnabled() const
{
    return m_settings.value(QStringLiteral("inferenceQaOffloadEnabled")).toBool(true);
}

bool SettingsController::inferenceShadowModeEnabled() const
{
    return m_settings.value(QStringLiteral("inferenceShadowModeEnabled")).toBool(false);
}

bool SettingsController::queryRouterEnabled() const
{
    return m_settings.value(QStringLiteral("queryRouterEnabled")).toBool(true);
}

bool SettingsController::fastEmbeddingEnabled() const
{
    return m_settings.value(QStringLiteral("fastEmbeddingEnabled")).toBool(true);
}

bool SettingsController::dualEmbeddingFusionEnabled() const
{
    return m_settings.value(QStringLiteral("dualEmbeddingFusionEnabled")).toBool(true);
}

bool SettingsController::rerankerCascadeEnabled() const
{
    return m_settings.value(QStringLiteral("rerankerCascadeEnabled")).toBool(true);
}

bool SettingsController::personalizedLtrEnabled() const
{
    return m_settings.value(QStringLiteral("personalizedLtrEnabled")).toBool(true);
}

double SettingsController::queryRouterMinConfidence() const
{
    return m_settings.value(QStringLiteral("queryRouterMinConfidence")).toDouble(0.45);
}

int SettingsController::strongEmbeddingTopK() const
{
    return m_settings.value(QStringLiteral("strongEmbeddingTopK")).toInt(40);
}

int SettingsController::fastEmbeddingTopK() const
{
    return m_settings.value(QStringLiteral("fastEmbeddingTopK")).toInt(60);
}

int SettingsController::rerankerStage1Max() const
{
    return m_settings.value(QStringLiteral("rerankerStage1Max")).toInt(40);
}

int SettingsController::rerankerStage2Max() const
{
    return m_settings.value(QStringLiteral("rerankerStage2Max")).toInt(12);
}

bool SettingsController::autoVectorMigration() const
{
    return m_settings.value(QStringLiteral("autoVectorMigration")).toBool(true);
}

double SettingsController::bm25WeightName() const
{
    return m_settings.value(QStringLiteral("bm25WeightName")).toDouble(10.0);
}

double SettingsController::bm25WeightPath() const
{
    return m_settings.value(QStringLiteral("bm25WeightPath")).toDouble(5.0);
}

double SettingsController::bm25WeightContent() const
{
    return m_settings.value(QStringLiteral("bm25WeightContent")).toDouble(1.0);
}

bool SettingsController::qaSnippetEnabled() const
{
    return m_settings.value(QStringLiteral("qaSnippetEnabled")).toBool(true);
}

int SettingsController::semanticBudgetMs() const
{
    return m_settings.value(QStringLiteral("semanticBudgetMs")).toInt(70);
}

int SettingsController::rerankBudgetMs() const
{
    return m_settings.value(QStringLiteral("rerankBudgetMs")).toInt(120);
}

int SettingsController::maxFileSizeMB() const
{
    return m_settings.value(QStringLiteral("maxFileSizeMB")).toInt(50);
}

int SettingsController::extractionTimeoutMs() const
{
    return m_settings.value(QStringLiteral("extractionTimeoutMs")).toInt(30000);
}

QStringList SettingsController::userPatterns() const
{
    return jsonArrayToStringList(m_settings.value(QStringLiteral("userPatterns")).toArray());
}

bool SettingsController::enableFeedbackLogging() const
{
    return m_settings.value(QStringLiteral("enableFeedbackLogging")).toBool(true);
}

bool SettingsController::enableInteractionTracking() const
{
    return m_settings.value(QStringLiteral("enableInteractionTracking")).toBool(false);
}

bool SettingsController::clipboardSignalEnabled() const
{
    return m_settings.value(QStringLiteral("clipboardSignalEnabled")).toBool(false);
}

int SettingsController::feedbackRetentionDays() const
{
    return m_settings.value(QStringLiteral("feedbackRetentionDays")).toInt(90);
}

QStringList SettingsController::sensitivePaths() const
{
    return jsonArrayToStringList(m_settings.value(QStringLiteral("sensitivePaths")).toArray());
}

QString SettingsController::theme() const
{
    return m_settings.value(QStringLiteral("theme")).toString(QStringLiteral("system"));
}

QString SettingsController::language() const
{
    return m_settings.value(QStringLiteral("language")).toString(QStringLiteral("en"));
}

QString SettingsController::platformStatusMessage() const
{
    return m_platformStatusMessage;
}

QString SettingsController::platformStatusKey() const
{
    return m_platformStatusKey;
}

bool SettingsController::platformStatusSuccess() const
{
    return m_platformStatusSuccess;
}

bool SettingsController::runtimeBoolSetting(const QString& key, bool defaultValue) const
{
    const QString normalizedKey = key.trimmed();
    if (normalizedKey.isEmpty()) {
        return defaultValue;
    }

    bool fallbackValue = defaultValue;
    if (m_settings.contains(normalizedKey)) {
        fallbackValue = jsonBoolValue(m_settings.value(normalizedKey), defaultValue);
    }

    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/betterspotlight/index.db");
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return fallbackValue;
    }

    static constexpr const char* kSelectSql =
        "SELECT value FROM settings WHERE key = ?1 LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSelectSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return fallbackValue;
    }

    const QByteArray keyUtf8 = normalizedKey.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);

    bool resolvedValue = fallbackValue;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* rawValue = sqlite3_column_text(stmt, 0);
        if (rawValue) {
            resolvedValue = parseBoolValue(
                QString::fromUtf8(reinterpret_cast<const char*>(rawValue)),
                fallbackValue);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return resolvedValue;
}

void SettingsController::setHotkey(const QString& value)
{
    if (hotkey() == value) {
        return;
    }
    m_settings[QStringLiteral("hotkey")] = value;
    saveSettings();
    emit hotkeyChanged();
    emit settingsChanged(QStringLiteral("hotkey"));
}

void SettingsController::setLaunchAtLogin(bool enabled)
{
    if (launchAtLogin() == enabled) {
        return;
    }
    const PlatformOperationResult result = m_platformIntegration
        ? m_platformIntegration->setLaunchAtLogin(enabled)
        : PlatformOperationResult{false, QStringLiteral("Platform integration is unavailable.")};
    if (!result.success) {
        setPlatformStatus(QStringLiteral("launchAtLogin"), false, result.message);
        emit launchAtLoginChanged();
        return;
    }

    m_settings[QStringLiteral("launchAtLogin")] = enabled;
    saveSettings();
    setPlatformStatus(QStringLiteral("launchAtLogin"), true,
                      result.message.isEmpty()
                          ? QStringLiteral("Launch-at-login preference applied.")
                          : result.message);
    emit launchAtLoginChanged();
    emit settingsChanged(QStringLiteral("launchAtLogin"));
}

void SettingsController::setShowInDock(bool enabled)
{
    if (showInDock() == enabled) {
        return;
    }
    const PlatformOperationResult result = m_platformIntegration
        ? m_platformIntegration->setShowInDock(enabled)
        : PlatformOperationResult{false, QStringLiteral("Platform integration is unavailable.")};
    if (!result.success) {
        setPlatformStatus(QStringLiteral("showInDock"), false, result.message);
        emit showInDockChanged();
        return;
    }

    m_settings[QStringLiteral("showInDock")] = enabled;
    saveSettings();
    setPlatformStatus(QStringLiteral("showInDock"), true,
                      result.message.isEmpty()
                          ? QStringLiteral("Dock visibility preference applied.")
                          : result.message);
    emit showInDockChanged();
    emit settingsChanged(QStringLiteral("showInDock"));
}

void SettingsController::setCheckForUpdates(bool enabled)
{
    if (checkForUpdates() == enabled) {
        return;
    }
    m_settings[QStringLiteral("checkForUpdates")] = enabled;
    saveSettings();
    emit checkForUpdatesChanged();
    emit settingsChanged(QStringLiteral("checkForUpdates"));
}

void SettingsController::setMaxResults(int max)
{
    const int clamped = std::clamp(max, 5, 200);
    if (maxResults() == clamped) {
        return;
    }
    m_settings[QStringLiteral("maxResults")] = clamped;
    saveSettings();
    emit maxResultsChanged();
    emit settingsChanged(QStringLiteral("maxResults"));
}

void SettingsController::setIndexRoots(const QVariantList& roots)
{
    const QJsonArray newRoots = variantListToJsonArray(roots);
    if (m_settings.value(QStringLiteral("indexRoots")).toArray() == newRoots) {
        return;
    }
    m_settings[QStringLiteral("indexRoots")] = newRoots;
    saveSettings();
    emit indexRootsChanged();
    emit settingsChanged(QStringLiteral("indexRoots"));
}

void SettingsController::setEnablePdf(bool enabled)
{
    if (enablePdf() == enabled) {
        return;
    }
    m_settings[QStringLiteral("enablePdf")] = enabled;
    saveSettings();
    emit enablePdfChanged();
    emit settingsChanged(QStringLiteral("enablePdf"));
}

void SettingsController::setEnableOcr(bool enabled)
{
    if (enableOcr() == enabled) {
        return;
    }
    m_settings[QStringLiteral("enableOcr")] = enabled;
    saveSettings();
    emit enableOcrChanged();
    emit settingsChanged(QStringLiteral("enableOcr"));
}

void SettingsController::setEmbeddingEnabled(bool enabled)
{
    if (embeddingEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("embeddingEnabled")] = enabled;
    saveSettings();
    emit embeddingEnabledChanged();
    emit settingsChanged(QStringLiteral("embeddingEnabled"));
}

void SettingsController::setInferenceServiceEnabled(bool enabled)
{
    if (inferenceServiceEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("inferenceServiceEnabled")] = enabled;
    saveSettings();
    emit inferenceServiceEnabledChanged();
    emit settingsChanged(QStringLiteral("inferenceServiceEnabled"));
}

void SettingsController::setInferenceEmbedOffloadEnabled(bool enabled)
{
    if (inferenceEmbedOffloadEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("inferenceEmbedOffloadEnabled")] = enabled;
    saveSettings();
    emit inferenceEmbedOffloadEnabledChanged();
    emit settingsChanged(QStringLiteral("inferenceEmbedOffloadEnabled"));
}

void SettingsController::setInferenceRerankOffloadEnabled(bool enabled)
{
    if (inferenceRerankOffloadEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("inferenceRerankOffloadEnabled")] = enabled;
    saveSettings();
    emit inferenceRerankOffloadEnabledChanged();
    emit settingsChanged(QStringLiteral("inferenceRerankOffloadEnabled"));
}

void SettingsController::setInferenceQaOffloadEnabled(bool enabled)
{
    if (inferenceQaOffloadEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("inferenceQaOffloadEnabled")] = enabled;
    saveSettings();
    emit inferenceQaOffloadEnabledChanged();
    emit settingsChanged(QStringLiteral("inferenceQaOffloadEnabled"));
}

void SettingsController::setInferenceShadowModeEnabled(bool enabled)
{
    if (inferenceShadowModeEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("inferenceShadowModeEnabled")] = enabled;
    saveSettings();
    emit inferenceShadowModeEnabledChanged();
    emit settingsChanged(QStringLiteral("inferenceShadowModeEnabled"));
}

void SettingsController::setQueryRouterEnabled(bool enabled)
{
    if (queryRouterEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("queryRouterEnabled")] = enabled;
    saveSettings();
    emit queryRouterEnabledChanged();
    emit settingsChanged(QStringLiteral("queryRouterEnabled"));
}

void SettingsController::setFastEmbeddingEnabled(bool enabled)
{
    if (fastEmbeddingEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("fastEmbeddingEnabled")] = enabled;
    saveSettings();
    emit fastEmbeddingEnabledChanged();
    emit settingsChanged(QStringLiteral("fastEmbeddingEnabled"));
}

void SettingsController::setDualEmbeddingFusionEnabled(bool enabled)
{
    if (dualEmbeddingFusionEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("dualEmbeddingFusionEnabled")] = enabled;
    saveSettings();
    emit dualEmbeddingFusionEnabledChanged();
    emit settingsChanged(QStringLiteral("dualEmbeddingFusionEnabled"));
}

void SettingsController::setRerankerCascadeEnabled(bool enabled)
{
    if (rerankerCascadeEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("rerankerCascadeEnabled")] = enabled;
    saveSettings();
    emit rerankerCascadeEnabledChanged();
    emit settingsChanged(QStringLiteral("rerankerCascadeEnabled"));
}

void SettingsController::setPersonalizedLtrEnabled(bool enabled)
{
    if (personalizedLtrEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("personalizedLtrEnabled")] = enabled;
    saveSettings();
    emit personalizedLtrEnabledChanged();
    emit settingsChanged(QStringLiteral("personalizedLtrEnabled"));
}

void SettingsController::setQueryRouterMinConfidence(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (std::abs(queryRouterMinConfidence() - clamped) < 0.0001) {
        return;
    }
    m_settings[QStringLiteral("queryRouterMinConfidence")] = clamped;
    saveSettings();
    emit queryRouterMinConfidenceChanged();
    emit settingsChanged(QStringLiteral("queryRouterMinConfidence"));
}

void SettingsController::setStrongEmbeddingTopK(int value)
{
    const int clamped = std::clamp(value, 1, 200);
    if (strongEmbeddingTopK() == clamped) {
        return;
    }
    m_settings[QStringLiteral("strongEmbeddingTopK")] = clamped;
    saveSettings();
    emit strongEmbeddingTopKChanged();
    emit settingsChanged(QStringLiteral("strongEmbeddingTopK"));
}

void SettingsController::setFastEmbeddingTopK(int value)
{
    const int clamped = std::clamp(value, 1, 300);
    if (fastEmbeddingTopK() == clamped) {
        return;
    }
    m_settings[QStringLiteral("fastEmbeddingTopK")] = clamped;
    saveSettings();
    emit fastEmbeddingTopKChanged();
    emit settingsChanged(QStringLiteral("fastEmbeddingTopK"));
}

void SettingsController::setRerankerStage1Max(int value)
{
    const int clamped = std::clamp(value, 4, 200);
    if (rerankerStage1Max() == clamped) {
        return;
    }
    m_settings[QStringLiteral("rerankerStage1Max")] = clamped;
    saveSettings();
    emit rerankerStage1MaxChanged();
    emit settingsChanged(QStringLiteral("rerankerStage1Max"));
}

void SettingsController::setRerankerStage2Max(int value)
{
    const int clamped = std::clamp(value, 4, 100);
    if (rerankerStage2Max() == clamped) {
        return;
    }
    m_settings[QStringLiteral("rerankerStage2Max")] = clamped;
    saveSettings();
    emit rerankerStage2MaxChanged();
    emit settingsChanged(QStringLiteral("rerankerStage2Max"));
}

void SettingsController::setAutoVectorMigration(bool enabled)
{
    if (autoVectorMigration() == enabled) {
        return;
    }
    m_settings[QStringLiteral("autoVectorMigration")] = enabled;
    saveSettings();
    emit autoVectorMigrationChanged();
    emit settingsChanged(QStringLiteral("autoVectorMigration"));
}

void SettingsController::setBm25WeightName(double value)
{
    const double clamped = std::max(0.0, value);
    if (std::abs(bm25WeightName() - clamped) < 0.0001) {
        return;
    }
    m_settings[QStringLiteral("bm25WeightName")] = clamped;
    saveSettings();
    emit bm25WeightNameChanged();
    emit settingsChanged(QStringLiteral("bm25WeightName"));
}

void SettingsController::setBm25WeightPath(double value)
{
    const double clamped = std::max(0.0, value);
    if (std::abs(bm25WeightPath() - clamped) < 0.0001) {
        return;
    }
    m_settings[QStringLiteral("bm25WeightPath")] = clamped;
    saveSettings();
    emit bm25WeightPathChanged();
    emit settingsChanged(QStringLiteral("bm25WeightPath"));
}

void SettingsController::setBm25WeightContent(double value)
{
    const double clamped = std::max(0.0, value);
    if (std::abs(bm25WeightContent() - clamped) < 0.0001) {
        return;
    }
    m_settings[QStringLiteral("bm25WeightContent")] = clamped;
    saveSettings();
    emit bm25WeightContentChanged();
    emit settingsChanged(QStringLiteral("bm25WeightContent"));
}

void SettingsController::setQaSnippetEnabled(bool enabled)
{
    if (qaSnippetEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("qaSnippetEnabled")] = enabled;
    saveSettings();
    emit qaSnippetEnabledChanged();
    emit settingsChanged(QStringLiteral("qaSnippetEnabled"));
}

void SettingsController::setSemanticBudgetMs(int ms)
{
    const int clamped = std::clamp(ms, 20, 500);
    if (semanticBudgetMs() == clamped) {
        return;
    }
    m_settings[QStringLiteral("semanticBudgetMs")] = clamped;
    saveSettings();
    emit semanticBudgetMsChanged();
    emit settingsChanged(QStringLiteral("semanticBudgetMs"));
}

void SettingsController::setRerankBudgetMs(int ms)
{
    const int clamped = std::clamp(ms, 40, 600);
    if (rerankBudgetMs() == clamped) {
        return;
    }
    m_settings[QStringLiteral("rerankBudgetMs")] = clamped;
    saveSettings();
    emit rerankBudgetMsChanged();
    emit settingsChanged(QStringLiteral("rerankBudgetMs"));
}

void SettingsController::setMaxFileSizeMB(int mb)
{
    const int clamped = std::clamp(mb, 1, 1024);
    if (maxFileSizeMB() == clamped) {
        return;
    }
    m_settings[QStringLiteral("maxFileSizeMB")] = clamped;
    saveSettings();
    emit maxFileSizeMBChanged();
    emit settingsChanged(QStringLiteral("maxFileSizeMB"));
}

void SettingsController::setExtractionTimeoutMs(int ms)
{
    const int clamped = std::clamp(ms, 1000, 120000);
    if (extractionTimeoutMs() == clamped) {
        return;
    }
    m_settings[QStringLiteral("extractionTimeoutMs")] = clamped;
    saveSettings();
    emit extractionTimeoutMsChanged();
    emit settingsChanged(QStringLiteral("extractionTimeoutMs"));
}

void SettingsController::setUserPatterns(const QStringList& patterns)
{
    if (userPatterns() == patterns) {
        return;
    }
    m_settings[QStringLiteral("userPatterns")] = stringListToJsonArray(patterns);
    saveSettings();
    emit userPatternsChanged();
    emit settingsChanged(QStringLiteral("userPatterns"));
}

void SettingsController::setEnableFeedbackLogging(bool enabled)
{
    if (enableFeedbackLogging() == enabled) {
        return;
    }
    m_settings[QStringLiteral("enableFeedbackLogging")] = enabled;
    saveSettings();
    emit enableFeedbackLoggingChanged();
    emit settingsChanged(QStringLiteral("enableFeedbackLogging"));
}

void SettingsController::setEnableInteractionTracking(bool enabled)
{
    if (enableInteractionTracking() == enabled) {
        return;
    }
    m_settings[QStringLiteral("enableInteractionTracking")] = enabled;
    saveSettings();
    emit enableInteractionTrackingChanged();
    emit settingsChanged(QStringLiteral("enableInteractionTracking"));
}

void SettingsController::setClipboardSignalEnabled(bool enabled)
{
    if (clipboardSignalEnabled() == enabled) {
        return;
    }
    m_settings[QStringLiteral("clipboardSignalEnabled")] = enabled;
    saveSettings();
    emit clipboardSignalEnabledChanged();
    emit settingsChanged(QStringLiteral("clipboardSignalEnabled"));
}

void SettingsController::setFeedbackRetentionDays(int days)
{
    const int clamped = std::clamp(days, 7, 365);
    if (feedbackRetentionDays() == clamped) {
        return;
    }
    m_settings[QStringLiteral("feedbackRetentionDays")] = clamped;
    saveSettings();
    emit feedbackRetentionDaysChanged();
    emit settingsChanged(QStringLiteral("feedbackRetentionDays"));
}

void SettingsController::setSensitivePaths(const QStringList& paths)
{
    if (sensitivePaths() == paths) {
        return;
    }
    m_settings[QStringLiteral("sensitivePaths")] = stringListToJsonArray(paths);
    saveSettings();
    emit sensitivePathsChanged();
    emit settingsChanged(QStringLiteral("sensitivePaths"));
}

void SettingsController::setTheme(const QString& value)
{
    if (theme() == value) {
        return;
    }
    m_settings[QStringLiteral("theme")] = value;
    saveSettings();
    emit themeChanged();
    emit settingsChanged(QStringLiteral("theme"));
}

void SettingsController::setLanguage(const QString& value)
{
    if (language() == value) {
        return;
    }
    m_settings[QStringLiteral("language")] = value;
    saveSettings();
    emit languageChanged();
    emit settingsChanged(QStringLiteral("language"));
}

void SettingsController::clearFeedbackData()
{
    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/betterspotlight/index.db");
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.toUtf8().constData(), &db) == SQLITE_OK && db) {
        sqlite3_exec(db, "DELETE FROM feedback", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM interactions", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM frequencies", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM behavior_events_v1", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM training_examples_v1", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM replay_reservoir_v1", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    m_settings[QStringLiteral("lastFeedbackAggregation")] = QStringLiteral("");
    saveSettings();
    emit feedbackDataCleared();
}

void SettingsController::exportData()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads.isEmpty()) {
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("exportedAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    payload[QStringLiteral("settings")] = m_settings;

    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/betterspotlight/index.db");
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && db) {
        auto exportTable = [&](const char* tableName) -> QJsonArray {
            QJsonArray rows;
            const QString sql = QStringLiteral("SELECT * FROM %1").arg(QString::fromUtf8(tableName));
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
                const int colCount = sqlite3_column_count(stmt);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    QJsonObject row;
                    for (int c = 0; c < colCount; ++c) {
                        const QString colName = QString::fromUtf8(sqlite3_column_name(stmt, c));
                        const int colType = sqlite3_column_type(stmt, c);
                        if (colType == SQLITE_INTEGER) {
                            row[colName] = static_cast<qint64>(sqlite3_column_int64(stmt, c));
                        } else if (colType == SQLITE_FLOAT) {
                            row[colName] = sqlite3_column_double(stmt, c);
                        } else if (colType == SQLITE_TEXT) {
                            row[colName] = QString::fromUtf8(
                                reinterpret_cast<const char*>(sqlite3_column_text(stmt, c)));
                        }
                    }
                    rows.append(row);
                }
                sqlite3_finalize(stmt);
            }
            return rows;
        };

        payload[QStringLiteral("feedback")] = exportTable("feedback");
        payload[QStringLiteral("interactions")] = exportTable("interactions");
        payload[QStringLiteral("frequencies")] = exportTable("frequencies");
        payload[QStringLiteral("behaviorEvents")] = exportTable("behavior_events_v1");
        payload[QStringLiteral("trainingExamples")] = exportTable("training_examples_v1");
        payload[QStringLiteral("replayReservoir")] = exportTable("replay_reservoir_v1");
        sqlite3_close(db);
    }

    QSaveFile file(downloads + QStringLiteral("/betterspotlight-data-export.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    file.commit();
}

void SettingsController::pauseIndexing()
{
    emit indexingPaused();
}

void SettingsController::resumeIndexing()
{
    emit indexingResumed();
}

void SettingsController::rebuildIndex()
{
    emit rebuildIndexRequested();
}

void SettingsController::rebuildVectorIndex()
{
    emit rebuildVectorIndexRequested();
}

void SettingsController::clearExtractionCache()
{
    emit clearExtractionCacheRequested();
}

void SettingsController::reindexFolder(const QString& folderPath)
{
    emit reindexFolderRequested(folderPath);
}

bool SettingsController::setRuntimeSetting(const QString& key, const QString& value)
{
    const QString normalizedKey = key.trimmed();
    if (normalizedKey.isEmpty()) {
        return false;
    }

    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/betterspotlight/index.db");
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    upsertSetting(db, normalizedKey, value);
    sqlite3_close(db);
    emit settingsChanged(normalizedKey);
    return true;
}

bool SettingsController::removeRuntimeSetting(const QString& key)
{
    const QString normalizedKey = key.trimmed();
    if (normalizedKey.isEmpty()) {
        return false;
    }

    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/betterspotlight/index.db");
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    static constexpr const char* kDeleteSql = "DELETE FROM settings WHERE key = ?1";
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, kDeleteSql, -1, &stmt, nullptr) == SQLITE_OK) {
        const QByteArray keyUtf8 = normalizedKey.toUtf8();
        sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (ok) {
        emit settingsChanged(normalizedKey);
    }
    return ok;
}

void SettingsController::loadSettings()
{
    QFile file(settingsFilePath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            m_settings = doc.object();
        }
    }

    const QString home = QDir::homePath();
    const QJsonArray existingIndexRoots =
        m_settings.value(QStringLiteral("indexRoots")).toArray();
    const bool hasLegacyHomeRoot = existingIndexRoots.size() == 1
        && existingIndexRoots.first().isObject()
        && existingIndexRoots.first().toObject().value(QStringLiteral("path")).toString() == home;
    if (existingIndexRoots.isEmpty() || hasLegacyHomeRoot) {
        const QJsonArray derivedRoots = indexRootsFromHomeDirectories(m_settings);
        if (!derivedRoots.isEmpty()) {
            m_settings[QStringLiteral("indexRoots")] = derivedRoots;
        }
    }

    ensureDefault(m_settings, QStringLiteral("hotkey"), QStringLiteral("Cmd+Space"));
    ensureDefault(m_settings, QStringLiteral("launchAtLogin"), false);
    ensureDefault(m_settings, QStringLiteral("showInDock"), false);
    ensureDefault(m_settings, QStringLiteral("checkForUpdates"), true);
    ensureDefault(m_settings, QStringLiteral("maxResults"), 20);
    ensureDefault(m_settings, QStringLiteral("indexRoots"), defaultIndexRoots());
    ensureDefault(m_settings, QStringLiteral("enablePdf"), true);
    ensureDefault(m_settings, QStringLiteral("enableOcr"), false);
    ensureDefault(m_settings, QStringLiteral("embeddingEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("inferenceServiceEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("inferenceEmbedOffloadEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("inferenceRerankOffloadEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("inferenceQaOffloadEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("inferenceShadowModeEnabled"), false);
    ensureDefault(m_settings, QStringLiteral("queryRouterEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("fastEmbeddingEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("dualEmbeddingFusionEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("rerankerCascadeEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("personalizedLtrEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("behaviorStreamEnabled"), false);
    ensureDefault(m_settings, QStringLiteral("learningEnabled"), false);
    ensureDefault(m_settings, QStringLiteral("learningPauseOnUserInput"), true);
    ensureDefault(m_settings, QStringLiteral("onlineRankerBlendAlpha"), 0.15);
    ensureDefault(m_settings, QStringLiteral("behaviorRawRetentionDays"), 30);
    ensureDefault(m_settings, QStringLiteral("queryRouterMinConfidence"), 0.45);
    ensureDefault(m_settings, QStringLiteral("strongEmbeddingTopK"), 40);
    ensureDefault(m_settings, QStringLiteral("fastEmbeddingTopK"), 60);
    ensureDefault(m_settings, QStringLiteral("rerankerStage1Max"), 40);
    ensureDefault(m_settings, QStringLiteral("rerankerStage2Max"), 12);
    ensureDefault(m_settings, QStringLiteral("autoVectorMigration"), true);
    ensureDefault(m_settings, QStringLiteral("bm25WeightName"), 10.0);
    ensureDefault(m_settings, QStringLiteral("bm25WeightPath"), 5.0);
    ensureDefault(m_settings, QStringLiteral("bm25WeightContent"), 1.0);
    ensureDefault(m_settings, QStringLiteral("qaSnippetEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("semanticBudgetMs"), 70);
    ensureDefault(m_settings, QStringLiteral("rerankBudgetMs"), 120);
    ensureDefault(m_settings, QStringLiteral("maxFileSizeMB"), 50);
    ensureDefault(m_settings, QStringLiteral("extractionTimeoutMs"), 30000);
    ensureDefault(m_settings, QStringLiteral("userPatterns"), QJsonArray{});
    ensureDefault(m_settings, QStringLiteral("enableFeedbackLogging"), true);
    ensureDefault(m_settings, QStringLiteral("enableInteractionTracking"), true);
    ensureDefault(m_settings, QStringLiteral("clipboardSignalEnabled"), false);
    ensureDefault(m_settings, QStringLiteral("feedbackRetentionDays"), 90);
    ensureDefault(m_settings, QStringLiteral("theme"), QStringLiteral("system"));
    ensureDefault(m_settings, QStringLiteral("language"), QStringLiteral("en"));
    ensureDefault(m_settings, QStringLiteral("sensitivePaths"), QJsonArray{
        home + QStringLiteral("/.ssh"),
        home + QStringLiteral("/.gnupg"),
        home + QStringLiteral("/.aws"),
        home + QStringLiteral("/Library/Keychains"),
        home + QStringLiteral("/Library/Preferences"),
    });

    saveSettings();
}

void SettingsController::saveSettings()
{
    const QString path = settingsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    file.write(QJsonDocument(m_settings).toJson(QJsonDocument::Indented));
    file.commit();
    syncRuntimeSettingsToDb(m_settings);
}

QString SettingsController::settingsFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/settings.json");
}

void SettingsController::setPlatformStatus(const QString& key, bool success, const QString& message)
{
    if (m_platformStatusKey == key
        && m_platformStatusSuccess == success
        && m_platformStatusMessage == message) {
        return;
    }

    m_platformStatusKey = key;
    m_platformStatusSuccess = success;
    m_platformStatusMessage = message;
    emit platformStatusChanged();
}

} // namespace bs
