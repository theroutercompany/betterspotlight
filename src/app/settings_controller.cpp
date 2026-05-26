#include "settings_controller.h"
#include "platform_integration.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <limits>

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

bool isValidRootMode(const QString& mode)
{
    return mode == QLatin1String("index_embed")
        || mode == QLatin1String("index_only")
        || mode == QLatin1String("skip");
}

QString normalizedRootMode(const QString& rawMode, const QString& fallback)
{
    const QString mode = rawMode.trimmed().toLower();
    if (mode == QLatin1String("index")) {
        return QStringLiteral("index_only");
    }
    if (isValidRootMode(mode)) {
        return mode;
    }
    return fallback;
}

bool isSafeHomeDirectoryName(const QString& name)
{
    return !name.isEmpty()
        && name != QLatin1String(".")
        && name != QLatin1String("..")
        && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'));
}

QString normalizedConfiguredRootPath(const QString& rawPath)
{
    QString path = rawPath.trimmed();
    if (path.startsWith(QStringLiteral("file://"))) {
        path = QUrl(path).toLocalFile();
    }
    path = QDir::cleanPath(path);
    if (path.isEmpty() || path == QLatin1String(".") || !QDir::isAbsolutePath(path)) {
        return {};
    }
    return path;
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
        const QString mode = normalizedRootMode(
            obj.value(QStringLiteral("mode")).toString(),
            QStringLiteral("index_only"));
        if (mode == QLatin1String("skip")) {
            continue;
        }
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (!isSafeHomeDirectoryName(name)) {
            continue;
        }
        roots.append(QJsonObject{
            {QStringLiteral("path"), home + QLatin1Char('/') + name},
            {QStringLiteral("mode"), mode},
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
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject obj = v.toObject();
        const QString path = normalizedConfiguredRootPath(
            obj.value(QStringLiteral("path")).toString());
        if (path.isEmpty()) {
            continue;
        }

        QVariantMap map;
        map.insert(QStringLiteral("path"), path);
        map.insert(QStringLiteral("mode"),
                   normalizedRootMode(obj.value(QStringLiteral("mode")).toString(),
                                      QStringLiteral("index_embed")));
        out.append(map);
    }
    return out;
}

QJsonArray variantListToJsonArray(const QVariantList& values)
{
    QJsonArray out;

    QSet<QString> seenPaths;
    for (const QVariant& v : values) {
        const QVariantMap map = v.toMap();
        const QString path = normalizedConfiguredRootPath(
            map.value(QStringLiteral("path")).toString());
        if (path.isEmpty() || seenPaths.contains(path)) {
            continue;
        }
        seenPaths.insert(path);

        QJsonObject obj;
        obj[QStringLiteral("path")] = path;
        obj[QStringLiteral("mode")] =
            normalizedRootMode(map.value(QStringLiteral("mode"), QStringLiteral("index_embed")).toString(),
                               QStringLiteral("index_embed"));
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

QString runtimeDbPath()
{
    const QString configuredDataDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_DATA_DIR").trimmed();
    const QString dataDir = configuredDataDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
              + QStringLiteral("/betterspotlight")
        : configuredDataDir;
    return QDir(dataDir).filePath(QStringLiteral("index.db"));
}

QString settingsDataDir()
{
    const QString configuredSettingsDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_SETTINGS_DIR").trimmed();
    if (!configuredSettingsDir.isEmpty()) {
        return configuredSettingsDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString exportDataDir()
{
    const QString configuredExportDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_EXPORT_DIR").trimmed();
    if (!configuredExportDir.isEmpty()) {
        return configuredExportDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

bool openRuntimeDb(sqlite3** db, int flags)
{
    if (!db) {
        return false;
    }
    *db = nullptr;

    const QString dbPath = runtimeDbPath();
    if ((flags & SQLITE_OPEN_CREATE) != 0) {
        QDir().mkpath(QFileInfo(dbPath).absolutePath());
    }

    if (sqlite3_open_v2(dbPath.toUtf8().constData(), db, flags, nullptr) != SQLITE_OK) {
        if (*db) {
            sqlite3_close(*db);
            *db = nullptr;
        }
        return false;
    }
    return true;
}

bool ensureSettingsTable(sqlite3* db)
{
    if (!db) {
        return false;
    }
    static constexpr const char* kSql =
        "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)";
    return sqlite3_exec(db, kSql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool execSql(sqlite3* db, const char* sql)
{
    if (!db || !sql) {
        return false;
    }
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool upsertSetting(sqlite3* db, const QString& key, const QString& value)
{
    if (!db) {
        return false;
    }
    static constexpr const char* kSql = R"(
        INSERT INTO settings (key, value) VALUES (?1, ?2)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    bool ok = sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT) == SQLITE_OK
        && sqlite3_bind_text(stmt, 2, valueUtf8.constData(), -1, SQLITE_TRANSIENT) == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

double jsonDoubleValue(const QJsonValue& value, double defaultValue)
{
    if (value.isDouble()) {
        const double parsed = value.toDouble(defaultValue);
        return std::isfinite(parsed) ? parsed : defaultValue;
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        return ok && std::isfinite(parsed) ? parsed : defaultValue;
    }
    return defaultValue;
}

int boundedJsonInt(const QJsonObject& settings,
                   const QString& key,
                   int defaultValue,
                   int minValue,
                   int maxValue)
{
    double value = jsonDoubleValue(settings.value(key), defaultValue);
    if (!std::isfinite(value)) {
        value = defaultValue;
    }
    return static_cast<int>(std::clamp(value,
                                       static_cast<double>(minValue),
                                       static_cast<double>(maxValue)));
}

double boundedFiniteDouble(double value,
                           double defaultValue,
                           double minValue,
                           double maxValue)
{
    if (!std::isfinite(value)) {
        value = defaultValue;
    }
    return std::clamp(value, minValue, maxValue);
}

double boundedJsonDouble(const QJsonObject& settings,
                         const QString& key,
                         double defaultValue,
                         double minValue,
                         double maxValue)
{
    return boundedFiniteDouble(jsonDoubleValue(settings.value(key), defaultValue),
                               defaultValue,
                               minValue,
                               maxValue);
}

double nonNegativeFiniteDouble(double value, double defaultValue)
{
    if (!std::isfinite(value)) {
        value = defaultValue;
    }
    return std::max(0.0, value);
}

double nonNegativeJsonDouble(const QJsonObject& settings,
                             const QString& key,
                             double defaultValue)
{
    return nonNegativeFiniteDouble(jsonDoubleValue(settings.value(key), defaultValue),
                                   defaultValue);
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

QString normalizedOnlineRankerRolloutMode(const QJsonValue& value)
{
    const QString mode = value.toString(QStringLiteral("instrumentation_only"))
        .trimmed()
        .toLower();
    if (mode == QLatin1String("instrumentation_only")
        || mode == QLatin1String("shadow_training")
        || mode == QLatin1String("blended_ranking")) {
        return mode;
    }
    return QStringLiteral("instrumentation_only");
}

void normalizeBoolSetting(QJsonObject& settings, const QString& key, bool defaultValue)
{
    settings[key] = jsonBoolValue(settings.value(key), defaultValue);
}

void normalizeBoundedIntSetting(QJsonObject& settings,
                                const QString& key,
                                int defaultValue,
                                int minValue,
                                int maxValue)
{
    settings[key] = boundedJsonInt(settings, key, defaultValue, minValue, maxValue);
}

void normalizeBoundedDoubleSetting(QJsonObject& settings,
                                   const QString& key,
                                   double defaultValue,
                                   double minValue,
                                   double maxValue)
{
    settings[key] = boundedJsonDouble(settings, key, defaultValue, minValue, maxValue);
}

void normalizeNonNegativeDoubleSetting(QJsonObject& settings,
                                       const QString& key,
                                       double defaultValue)
{
    settings[key] = nonNegativeJsonDouble(settings, key, defaultValue);
}

void normalizeSettings(QJsonObject& settings)
{
    const QJsonArray normalizedRoots =
        variantListToJsonArray(jsonArrayToVariantList(
            settings.value(QStringLiteral("indexRoots")).toArray()));
    settings[QStringLiteral("indexRoots")] =
        normalizedRoots.isEmpty() ? defaultIndexRoots() : normalizedRoots;

    normalizeBoolSetting(settings, QStringLiteral("launchAtLogin"), false);
    normalizeBoolSetting(settings, QStringLiteral("showInDock"), false);
    normalizeBoolSetting(settings, QStringLiteral("checkForUpdates"), true);
    normalizeBoolSetting(settings, QStringLiteral("enablePdf"), true);
    normalizeBoolSetting(settings, QStringLiteral("enableOcr"), false);
    normalizeBoolSetting(settings, QStringLiteral("embeddingEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("inferenceServiceEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("inferenceEmbedOffloadEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("inferenceRerankOffloadEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("inferenceQaOffloadEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("inferenceShadowModeEnabled"), false);
    normalizeBoolSetting(settings, QStringLiteral("queryRouterEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("fastEmbeddingEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("dualEmbeddingFusionEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("rerankerCascadeEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("personalizedLtrEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("behaviorStreamEnabled"), false);
    normalizeBoolSetting(settings, QStringLiteral("learningEnabled"), false);
    normalizeBoolSetting(settings, QStringLiteral("behaviorCaptureAppActivityEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("behaviorCaptureInputActivityEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("behaviorCaptureSearchEventsEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("behaviorCaptureWindowTitleHashEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("behaviorCaptureBrowserHostHashEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("learningPauseOnUserInput"), true);
    normalizeBoolSetting(settings, QStringLiteral("autoVectorMigration"), true);
    normalizeBoolSetting(settings, QStringLiteral("qaSnippetEnabled"), true);
    normalizeBoolSetting(settings, QStringLiteral("enableFeedbackLogging"), true);
    normalizeBoolSetting(settings, QStringLiteral("enableInteractionTracking"), true);
    normalizeBoolSetting(settings, QStringLiteral("clipboardSignalEnabled"), false);

    normalizeBoundedIntSetting(settings, QStringLiteral("maxResults"), 20, 5, 200);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("queryRouterMinConfidence"),
                                  0.45,
                                  0.0,
                                  1.0);
    normalizeBoundedIntSetting(settings, QStringLiteral("strongEmbeddingTopK"), 40, 1, 200);
    normalizeBoundedIntSetting(settings, QStringLiteral("fastEmbeddingTopK"), 60, 1, 300);
    normalizeBoundedIntSetting(settings, QStringLiteral("rerankerStage1Max"), 40, 4, 200);
    normalizeBoundedIntSetting(settings, QStringLiteral("rerankerStage2Max"), 12, 4, 100);
    normalizeNonNegativeDoubleSetting(settings, QStringLiteral("bm25WeightName"), 10.0);
    normalizeNonNegativeDoubleSetting(settings, QStringLiteral("bm25WeightPath"), 5.0);
    normalizeNonNegativeDoubleSetting(settings, QStringLiteral("bm25WeightContent"), 1.0);
    normalizeBoundedIntSetting(settings, QStringLiteral("semanticBudgetMs"), 350, 20, 500);
    normalizeBoundedIntSetting(settings, QStringLiteral("rerankBudgetMs"), 600, 40, 600);
    normalizeBoundedIntSetting(settings, QStringLiteral("maxFileSizeMB"), 50, 1, 1024);
    normalizeBoundedIntSetting(settings,
                               QStringLiteral("extractionTimeoutMs"),
                               30000,
                               1000,
                               120000);
    normalizeBoundedIntSetting(settings, QStringLiteral("feedbackRetentionDays"), 90, 7, 365);

    const int maxInt = std::numeric_limits<int>::max();
    normalizeBoundedIntSetting(settings,
                               QStringLiteral("onlineRankerHealthWindowDays"),
                               7,
                               1,
                               maxInt);
    normalizeBoundedIntSetting(settings,
                               QStringLiteral("onlineRankerRecentCycleHistoryLimit"),
                               50,
                               1,
                               maxInt);
    normalizeBoundedIntSetting(settings,
                               QStringLiteral("onlineRankerPromotionGateMinPositives"),
                               80,
                               1,
                               maxInt);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                                  0.5,
                                  0.0,
                                  1.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                                  0.1,
                                  0.0,
                                  1.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                                  2500.0,
                                  10.0,
                                  1000000.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                                  35.0,
                                  0.0,
                                  1000.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                                  0.05,
                                  0.0,
                                  1.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                                  0.995,
                                  0.0,
                                  1.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerBlendAlpha"),
                                  0.15,
                                  0.0,
                                  1.0);
    normalizeBoundedDoubleSetting(settings,
                                  QStringLiteral("onlineRankerNegativeSampleRatio"),
                                  3.0,
                                  0.0,
                                  10.0);
    normalizeBoundedIntSetting(settings,
                               QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                               1200,
                               60,
                               maxInt);
    normalizeBoundedIntSetting(settings,
                               QStringLiteral("behaviorRawRetentionDays"),
                               30,
                               1,
                               maxInt);
    settings[QStringLiteral("onlineRankerRolloutMode")] =
        normalizedOnlineRankerRolloutMode(settings.value(QStringLiteral("onlineRankerRolloutMode")));
}

void syncRuntimeSettingsToDb(const QJsonObject& rawSettings)
{
    QJsonObject settings = rawSettings;
    normalizeSettings(settings);

    sqlite3* db = nullptr;
    if (!openRuntimeDb(&db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) {
        return;
    }
    if (!ensureSettingsTable(db)) {
        sqlite3_close(db);
        return;
    }

    auto writeSetting = [&](const QString& key, const QString& value) {
        upsertSetting(db, key, value);
    };

    writeSetting(QStringLiteral("embeddingEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("embeddingEnabled")).toBool(true)));
    writeSetting(QStringLiteral("inferenceServiceEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("inferenceServiceEnabled")).toBool(true)));
    writeSetting(QStringLiteral("inferenceEmbedOffloadEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("inferenceEmbedOffloadEnabled")).toBool(true)));
    writeSetting(QStringLiteral("inferenceRerankOffloadEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("inferenceRerankOffloadEnabled")).toBool(true)));
    writeSetting(QStringLiteral("inferenceQaOffloadEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("inferenceQaOffloadEnabled")).toBool(true)));
    writeSetting(QStringLiteral("inferenceShadowModeEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("inferenceShadowModeEnabled")).toBool(false)));
    writeSetting(QStringLiteral("queryRouterEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("queryRouterEnabled")).toBool(true)));
    writeSetting(QStringLiteral("queryRouterMinConfidence"),
                 QString::number(boundedJsonDouble(settings,
                                                   QStringLiteral("queryRouterMinConfidence"),
                                                   0.45,
                                                   0.0,
                                                   1.0),
                                 'f',
                                 2));
    writeSetting(QStringLiteral("fastEmbeddingEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("fastEmbeddingEnabled")).toBool(true)));
    writeSetting(QStringLiteral("dualEmbeddingFusionEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("dualEmbeddingFusionEnabled")).toBool(true)));
    writeSetting(QStringLiteral("strongEmbeddingTopK"),
                 QString::number(settings.value(QStringLiteral("strongEmbeddingTopK")).toInt(40)));
    writeSetting(QStringLiteral("fastEmbeddingTopK"),
                 QString::number(settings.value(QStringLiteral("fastEmbeddingTopK")).toInt(60)));
    writeSetting(QStringLiteral("rerankerCascadeEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("rerankerCascadeEnabled")).toBool(true)));
    writeSetting(QStringLiteral("rerankerStage1Max"),
                 QString::number(settings.value(QStringLiteral("rerankerStage1Max")).toInt(40)));
    writeSetting(QStringLiteral("rerankerStage2Max"),
                 QString::number(settings.value(QStringLiteral("rerankerStage2Max")).toInt(12)));
    writeSetting(QStringLiteral("autoVectorMigration"),
                 boolToSqlValue(settings.value(QStringLiteral("autoVectorMigration")).toBool(true)));
    writeSetting(QStringLiteral("bm25WeightName"),
                 QString::number(nonNegativeJsonDouble(settings,
                                                       QStringLiteral("bm25WeightName"),
                                                       10.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("bm25WeightPath"),
                 QString::number(nonNegativeJsonDouble(settings,
                                                       QStringLiteral("bm25WeightPath"),
                                                       5.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("bm25WeightContent"),
                 QString::number(nonNegativeJsonDouble(settings,
                                                       QStringLiteral("bm25WeightContent"),
                                                       1.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("qaSnippetEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("qaSnippetEnabled")).toBool(true)));
    writeSetting(QStringLiteral("personalizedLtrEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("personalizedLtrEnabled")).toBool(true)));
    writeSetting(QStringLiteral("behaviorStreamEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("behaviorStreamEnabled")).toBool(false)));
    writeSetting(QStringLiteral("learningEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("learningEnabled")).toBool(false)));
    writeSetting(QStringLiteral("behaviorCaptureAppActivityEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("behaviorCaptureAppActivityEnabled")).toBool(true)));
    writeSetting(QStringLiteral("behaviorCaptureInputActivityEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("behaviorCaptureInputActivityEnabled")).toBool(true)));
    writeSetting(QStringLiteral("behaviorCaptureSearchEventsEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("behaviorCaptureSearchEventsEnabled")).toBool(true)));
    writeSetting(QStringLiteral("behaviorCaptureWindowTitleHashEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("behaviorCaptureWindowTitleHashEnabled")).toBool(true)));
    writeSetting(QStringLiteral("behaviorCaptureBrowserHostHashEnabled"),
                 boolToSqlValue(settings.value(QStringLiteral("behaviorCaptureBrowserHostHashEnabled")).toBool(true)));
    writeSetting(QStringLiteral("onlineRankerRolloutMode"),
                 settings.value(QStringLiteral("onlineRankerRolloutMode"))
                     .toString(QStringLiteral("instrumentation_only"))
                     .trimmed()
                     .toLower());
    writeSetting(QStringLiteral("onlineRankerHealthWindowDays"),
                 QString::number(std::max(
                     1, settings.value(QStringLiteral("onlineRankerHealthWindowDays")).toInt(7))));
    writeSetting(QStringLiteral("onlineRankerRecentCycleHistoryLimit"),
                 QString::number(std::max(
                     1, settings.value(QStringLiteral("onlineRankerRecentCycleHistoryLimit")).toInt(50))));
    writeSetting(QStringLiteral("onlineRankerPromotionGateMinPositives"),
                 QString::number(std::max(
                     1, settings.value(QStringLiteral("onlineRankerPromotionGateMinPositives")).toInt(80))));
    writeSetting(QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerPromotionMinAttributedRate")).toDouble(0.5),
                     0.0,
                     1.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerPromotionMinContextDigestRate")).toDouble(0.1),
                     0.0,
                     1.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerPromotionLatencyUsMax")).toDouble(2500.0),
                     10.0,
                     1000000.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax")).toDouble(35.0),
                     0.0,
                     1000.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerPromotionPredictionFailureRateMax")).toDouble(0.05),
                     0.0,
                     1.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerPromotionSaturationRateMax")).toDouble(0.995),
                     0.0,
                     1.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("learningPauseOnUserInput"),
                 boolToSqlValue(settings.value(QStringLiteral("learningPauseOnUserInput")).toBool(true)));
    writeSetting(QStringLiteral("onlineRankerBlendAlpha"),
                 QString::number(settings.value(QStringLiteral("onlineRankerBlendAlpha")).toDouble(0.15), 'g', 17));
    writeSetting(QStringLiteral("onlineRankerNegativeSampleRatio"),
                 QString::number(std::clamp(
                     settings.value(QStringLiteral("onlineRankerNegativeSampleRatio")).toDouble(3.0),
                     0.0,
                     10.0),
                                 'g',
                                 17));
    writeSetting(QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                 QString::number(std::max(
                     60, settings.value(QStringLiteral("onlineRankerMaxTrainingBatchSize")).toInt(1200))));
    writeSetting(QStringLiteral("behaviorRawRetentionDays"),
                 QString::number(settings.value(QStringLiteral("behaviorRawRetentionDays")).toInt(30)));
    writeSetting(QStringLiteral("semanticBudgetMs"),
                 QString::number(settings.value(QStringLiteral("semanticBudgetMs")).toInt(350)));
    writeSetting(QStringLiteral("rerankBudgetMs"),
                 QString::number(settings.value(QStringLiteral("rerankBudgetMs")).toInt(600)));
    const qint64 maxFileSizeBytes =
        static_cast<qint64>(boundedJsonInt(
            settings, QStringLiteral("maxFileSizeMB"), 50, 1, 1024))
        * 1024LL * 1024LL;
    writeSetting(QStringLiteral("max_file_size"), QString::number(maxFileSizeBytes));
    writeSetting(QStringLiteral("extraction_timeout_ms"),
                 QString::number(boundedJsonInt(
                     settings, QStringLiteral("extractionTimeoutMs"), 30000, 1000, 120000)));
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
    return jsonBoolValue(m_settings.value(QStringLiteral("launchAtLogin")), false);
}

bool SettingsController::showInDock() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("showInDock")), false);
}

bool SettingsController::checkForUpdates() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("checkForUpdates")), true);
}

int SettingsController::maxResults() const
{
    return boundedJsonInt(m_settings, QStringLiteral("maxResults"), 20, 5, 200);
}

QVariantList SettingsController::indexRoots() const
{
    return jsonArrayToVariantList(m_settings.value(QStringLiteral("indexRoots")).toArray());
}

bool SettingsController::enablePdf() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("enablePdf")), true);
}

bool SettingsController::enableOcr() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("enableOcr")), false);
}

bool SettingsController::embeddingEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("embeddingEnabled")), true);
}

bool SettingsController::inferenceServiceEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("inferenceServiceEnabled")), true);
}

bool SettingsController::inferenceEmbedOffloadEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("inferenceEmbedOffloadEnabled")), true);
}

bool SettingsController::inferenceRerankOffloadEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("inferenceRerankOffloadEnabled")), true);
}

bool SettingsController::inferenceQaOffloadEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("inferenceQaOffloadEnabled")), true);
}

bool SettingsController::inferenceShadowModeEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("inferenceShadowModeEnabled")), false);
}

bool SettingsController::queryRouterEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("queryRouterEnabled")), true);
}

bool SettingsController::fastEmbeddingEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("fastEmbeddingEnabled")), true);
}

bool SettingsController::dualEmbeddingFusionEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("dualEmbeddingFusionEnabled")), true);
}

bool SettingsController::rerankerCascadeEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("rerankerCascadeEnabled")), true);
}

bool SettingsController::personalizedLtrEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("personalizedLtrEnabled")), true);
}

double SettingsController::queryRouterMinConfidence() const
{
    return boundedJsonDouble(m_settings,
                             QStringLiteral("queryRouterMinConfidence"),
                             0.45,
                             0.0,
                             1.0);
}

int SettingsController::strongEmbeddingTopK() const
{
    return boundedJsonInt(m_settings, QStringLiteral("strongEmbeddingTopK"), 40, 1, 200);
}

int SettingsController::fastEmbeddingTopK() const
{
    return boundedJsonInt(m_settings, QStringLiteral("fastEmbeddingTopK"), 60, 1, 300);
}

int SettingsController::rerankerStage1Max() const
{
    return boundedJsonInt(m_settings, QStringLiteral("rerankerStage1Max"), 40, 4, 200);
}

int SettingsController::rerankerStage2Max() const
{
    return boundedJsonInt(m_settings, QStringLiteral("rerankerStage2Max"), 12, 4, 100);
}

bool SettingsController::autoVectorMigration() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("autoVectorMigration")), true);
}

double SettingsController::bm25WeightName() const
{
    return nonNegativeJsonDouble(m_settings, QStringLiteral("bm25WeightName"), 10.0);
}

double SettingsController::bm25WeightPath() const
{
    return nonNegativeJsonDouble(m_settings, QStringLiteral("bm25WeightPath"), 5.0);
}

double SettingsController::bm25WeightContent() const
{
    return nonNegativeJsonDouble(m_settings, QStringLiteral("bm25WeightContent"), 1.0);
}

bool SettingsController::qaSnippetEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("qaSnippetEnabled")), true);
}

int SettingsController::semanticBudgetMs() const
{
    return boundedJsonInt(m_settings, QStringLiteral("semanticBudgetMs"), 350, 20, 500);
}

int SettingsController::rerankBudgetMs() const
{
    return boundedJsonInt(m_settings, QStringLiteral("rerankBudgetMs"), 600, 40, 600);
}

int SettingsController::maxFileSizeMB() const
{
    return boundedJsonInt(m_settings, QStringLiteral("maxFileSizeMB"), 50, 1, 1024);
}

int SettingsController::extractionTimeoutMs() const
{
    return boundedJsonInt(m_settings,
                          QStringLiteral("extractionTimeoutMs"),
                          30000,
                          1000,
                          120000);
}

QStringList SettingsController::userPatterns() const
{
    return jsonArrayToStringList(m_settings.value(QStringLiteral("userPatterns")).toArray());
}

bool SettingsController::enableFeedbackLogging() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("enableFeedbackLogging")), true);
}

bool SettingsController::enableInteractionTracking() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("enableInteractionTracking")), true);
}

bool SettingsController::clipboardSignalEnabled() const
{
    return jsonBoolValue(m_settings.value(QStringLiteral("clipboardSignalEnabled")), false);
}

int SettingsController::feedbackRetentionDays() const
{
    return boundedJsonInt(m_settings, QStringLiteral("feedbackRetentionDays"), 90, 7, 365);
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

    sqlite3* db = nullptr;
    if (!openRuntimeDb(&db, SQLITE_OPEN_READONLY)) {
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
    const double clamped = boundedFiniteDouble(value, 0.45, 0.0, 1.0);
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
    const double clamped = nonNegativeFiniteDouble(value, 10.0);
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
    const double clamped = nonNegativeFiniteDouble(value, 5.0);
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
    const double clamped = nonNegativeFiniteDouble(value, 1.0);
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

bool SettingsController::clearFeedbackData()
{
    sqlite3* db = nullptr;
    if (!openRuntimeDb(&db, SQLITE_OPEN_READWRITE)) {
        setPlatformStatus(QStringLiteral("clearFeedbackData"),
                          false,
                          QStringLiteral("Could not open the local index database."));
        return false;
    }

    const char* tables[] = {
        "feedback",
        "interactions",
        "frequencies",
        "behavior_events_v1",
        "training_examples_v1",
        "replay_reservoir_v1",
    };

    bool ok = execSql(db, "SAVEPOINT clear_feedback_data");
    for (const char* table : tables) {
        if (!ok) {
            break;
        }
        const QString sql = QStringLiteral("DELETE FROM %1").arg(QString::fromUtf8(table));
        ok = execSql(db, sql.toUtf8().constData());
    }
    ok = ok && execSql(db, "RELEASE clear_feedback_data");
    if (!ok) {
        execSql(db, "ROLLBACK TO clear_feedback_data");
        execSql(db, "RELEASE clear_feedback_data");
    }
    sqlite3_close(db);

    if (!ok) {
        setPlatformStatus(QStringLiteral("clearFeedbackData"),
                          false,
                          QStringLiteral("Failed to clear feedback data from the local index database."));
        return false;
    }

    m_settings[QStringLiteral("lastFeedbackAggregation")] = QStringLiteral("");
    saveSettings();
    setPlatformStatus(QStringLiteral("clearFeedbackData"),
                      true,
                      QStringLiteral("Feedback data cleared."));
    emit feedbackDataCleared();
    return true;
}

bool SettingsController::exportData()
{
    const QString downloads = exportDataDir();
    if (downloads.isEmpty()) {
        setPlatformStatus(QStringLiteral("exportData"),
                          false,
                          QStringLiteral("Download location is unavailable."));
        return false;
    }

    QJsonObject payload;
    payload[QStringLiteral("exportedAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    payload[QStringLiteral("settings")] = m_settings;

    sqlite3* db = nullptr;
    if (!openRuntimeDb(&db, SQLITE_OPEN_READONLY)) {
        setPlatformStatus(QStringLiteral("exportData"),
                          false,
                          QStringLiteral("Could not open the local index database."));
        return false;
    }

    bool exportOk = true;
    auto exportTable = [&](const char* tableName) -> QJsonArray {
        QJsonArray rows;
        const QString sql = QStringLiteral("SELECT * FROM %1").arg(QString::fromUtf8(tableName));
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
            const int colCount = sqlite3_column_count(stmt);
            int rc = SQLITE_ROW;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
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
            if (rc != SQLITE_DONE) {
                exportOk = false;
            }
            sqlite3_finalize(stmt);
        } else {
            exportOk = false;
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

    if (!exportOk) {
        setPlatformStatus(QStringLiteral("exportData"),
                          false,
                          QStringLiteral("Failed to read all export tables from the local index database."));
        return false;
    }

    QSaveFile file(downloads + QStringLiteral("/betterspotlight-data-export.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setPlatformStatus(QStringLiteral("exportData"),
                          false,
                          QStringLiteral("Could not create the export file in Downloads."));
        return false;
    }

    const QByteArray data = QJsonDocument(payload).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        setPlatformStatus(QStringLiteral("exportData"),
                          false,
                          QStringLiteral("Failed to write the export file in Downloads."));
        return false;
    }

    setPlatformStatus(QStringLiteral("exportData"),
                      true,
                      QStringLiteral("Data exported to ~/Downloads/betterspotlight-data-export.json."));
    return true;
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

    sqlite3* db = nullptr;
    if (!openRuntimeDb(&db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) {
        return false;
    }

    const bool ok = ensureSettingsTable(db) && upsertSetting(db, normalizedKey, value);
    sqlite3_close(db);
    if (ok) {
        emit settingsChanged(normalizedKey);
    }
    return ok;
}

bool SettingsController::removeRuntimeSetting(const QString& key)
{
    const QString normalizedKey = key.trimmed();
    if (normalizedKey.isEmpty()) {
        return false;
    }

    sqlite3* db = nullptr;
    if (!openRuntimeDb(&db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) {
        return false;
    }

    static constexpr const char* kDeleteSql = "DELETE FROM settings WHERE key = ?1";
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (ensureSettingsTable(db)
        && sqlite3_prepare_v2(db, kDeleteSql, -1, &stmt, nullptr) == SQLITE_OK) {
        const QByteArray keyUtf8 = normalizedKey.toUtf8();
        ok = sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_DONE;
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
    ensureDefault(m_settings, QStringLiteral("behaviorCaptureAppActivityEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("behaviorCaptureInputActivityEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("behaviorCaptureSearchEventsEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("behaviorCaptureWindowTitleHashEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("behaviorCaptureBrowserHostHashEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("onlineRankerRolloutMode"),
                  QStringLiteral("instrumentation_only"));
    ensureDefault(m_settings, QStringLiteral("onlineRankerHealthWindowDays"), 7);
    ensureDefault(m_settings, QStringLiteral("onlineRankerRecentCycleHistoryLimit"), 50);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionGateMinPositives"), 80);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionMinAttributedRate"), 0.5);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionMinContextDigestRate"), 0.1);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionLatencyUsMax"), 2500.0);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"), 35.0);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"), 0.05);
    ensureDefault(m_settings, QStringLiteral("onlineRankerPromotionSaturationRateMax"), 0.995);
    ensureDefault(m_settings, QStringLiteral("learningPauseOnUserInput"), true);
    ensureDefault(m_settings, QStringLiteral("onlineRankerBlendAlpha"), 0.15);
    ensureDefault(m_settings, QStringLiteral("onlineRankerNegativeSampleRatio"), 3.0);
    ensureDefault(m_settings, QStringLiteral("onlineRankerMaxTrainingBatchSize"), 1200);
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
    ensureDefault(m_settings, QStringLiteral("semanticBudgetMs"), 350);
    ensureDefault(m_settings, QStringLiteral("rerankBudgetMs"), 600);
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

    normalizeSettings(m_settings);
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
    return QDir(settingsDataDir()).filePath(QStringLiteral("settings.json"));
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
