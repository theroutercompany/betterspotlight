#include "settings_controller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariantMap>

#include <sqlite3.h>

#include <algorithm>

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
        {QStringLiteral("mode"), QStringLiteral("index_only")},
    });
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

} // namespace

SettingsController::SettingsController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
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

int SettingsController::maxFileSizeMB() const
{
    return m_settings.value(QStringLiteral("maxFileSizeMB")).toInt(50);
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
    m_settings[QStringLiteral("launchAtLogin")] = enabled;
    saveSettings();
    emit launchAtLoginChanged();
    emit settingsChanged(QStringLiteral("launchAtLogin"));
}

void SettingsController::setShowInDock(bool enabled)
{
    if (showInDock() == enabled) {
        return;
    }
    m_settings[QStringLiteral("showInDock")] = enabled;
    saveSettings();
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
    ensureDefault(m_settings, QStringLiteral("hotkey"), QStringLiteral("Cmd+Space"));
    ensureDefault(m_settings, QStringLiteral("launchAtLogin"), false);
    ensureDefault(m_settings, QStringLiteral("showInDock"), false);
    ensureDefault(m_settings, QStringLiteral("checkForUpdates"), true);
    ensureDefault(m_settings, QStringLiteral("maxResults"), 20);
    ensureDefault(m_settings, QStringLiteral("indexRoots"), defaultIndexRoots());
    ensureDefault(m_settings, QStringLiteral("enablePdf"), true);
    ensureDefault(m_settings, QStringLiteral("enableOcr"), false);
    ensureDefault(m_settings, QStringLiteral("embeddingEnabled"), true);
    ensureDefault(m_settings, QStringLiteral("maxFileSizeMB"), 50);
    ensureDefault(m_settings, QStringLiteral("userPatterns"), QJsonArray{});
    ensureDefault(m_settings, QStringLiteral("enableFeedbackLogging"), true);
    ensureDefault(m_settings, QStringLiteral("enableInteractionTracking"), true);
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
}

QString SettingsController::settingsFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/settings.json");
}

} // namespace bs
