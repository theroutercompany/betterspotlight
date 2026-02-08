#include "core/shared/settings_manager.h"
#include "core/shared/logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>

namespace bs {

std::optional<Settings> SettingsManager::load()
{
    const QString filePath = settingsFilePath();
    QFile file(filePath);
    if (!file.exists()) {
        return std::nullopt;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN(bsCore, "Failed to open settings file for read: %s", qUtf8Printable(filePath));
        return std::nullopt;
    }

    const QByteArray rawJson = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        LOG_WARN(bsCore,
                 "Failed to parse settings JSON (%s): %s",
                 qUtf8Printable(filePath),
                 qUtf8Printable(parseError.errorString()));
        return std::nullopt;
    }

    return fromJson(doc.object());
}

bool SettingsManager::save(const Settings& settings)
{
    const QString filePath = settingsFilePath();
    const QFileInfo fileInfo(filePath);
    const QString parentDir = fileInfo.absolutePath();

    if (!QDir().mkpath(parentDir)) {
        LOG_ERROR(bsCore, "Failed to create settings directory: %s", qUtf8Printable(parentDir));
        return false;
    }

    const QJsonDocument doc(toJson(settings));
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_ERROR(bsCore, "Failed to open settings file for write: %s", qUtf8Printable(filePath));
        return false;
    }

    const qint64 bytesWritten = file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    if (bytesWritten < 0) {
        LOG_ERROR(bsCore, "Failed to write settings file: %s", qUtf8Printable(filePath));
        return false;
    }

    return true;
}

QString SettingsManager::settingsFilePath()
{
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return basePath + QStringLiteral("/betterspotlight/settings.json");
}

QJsonObject SettingsManager::toJson(const Settings& settings)
{
    QJsonObject json;
    json.insert(QStringLiteral("dbPath"), settings.dbPath);
    json.insert(QStringLiteral("indexPaths"), QJsonArray::fromStringList(settings.indexPaths));
    json.insert(QStringLiteral("excludePatterns"), QJsonArray::fromStringList(settings.excludePatterns));
    json.insert(QStringLiteral("maxFileSize"), static_cast<qint64>(settings.maxFileSize));
    json.insert(QStringLiteral("extractionTimeoutMs"), static_cast<int>(settings.extractionTimeoutMs));
    json.insert(QStringLiteral("chunkSizeBytes"), static_cast<int>(settings.chunkSizeBytes));
    json.insert(QStringLiteral("embeddingEnabled"), settings.embeddingEnabled);
    return json;
}

Settings SettingsManager::fromJson(const QJsonObject& json)
{
    Settings settings;

    settings.dbPath = json.value(QStringLiteral("dbPath")).toString(settings.dbPath);

    const QJsonArray indexPathsArray = json.value(QStringLiteral("indexPaths")).toArray();
    settings.indexPaths.clear();
    settings.indexPaths.reserve(indexPathsArray.size());
    for (const QJsonValue& value : indexPathsArray) {
        settings.indexPaths.append(value.toString());
    }

    const QJsonArray excludePatternsArray = json.value(QStringLiteral("excludePatterns")).toArray();
    settings.excludePatterns.clear();
    settings.excludePatterns.reserve(excludePatternsArray.size());
    for (const QJsonValue& value : excludePatternsArray) {
        settings.excludePatterns.append(value.toString());
    }

    if (json.contains(QStringLiteral("maxFileSize"))) {
        settings.maxFileSize = static_cast<int64_t>(
            json.value(QStringLiteral("maxFileSize")).toVariant().toLongLong());
    }

    if (json.contains(QStringLiteral("extractionTimeoutMs"))) {
        settings.extractionTimeoutMs = json.value(QStringLiteral("extractionTimeoutMs"))
                                           .toVariant()
                                           .toUInt();
    }

    if (json.contains(QStringLiteral("chunkSizeBytes"))) {
        settings.chunkSizeBytes = json.value(QStringLiteral("chunkSizeBytes"))
                                      .toVariant()
                                      .toUInt();
    }

    settings.embeddingEnabled = json.value(QStringLiteral("embeddingEnabled"))
                                    .toBool(settings.embeddingEnabled);

    return settings;
}

} // namespace bs
