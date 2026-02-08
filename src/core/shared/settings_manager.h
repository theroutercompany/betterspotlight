#pragma once

#include "core/shared/settings.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace bs {

// SettingsManager -- JSON save/load for application settings.
//
// Settings are stored as a JSON file at:
//   ~/Library/Application Support/betterspotlight/settings.json
class SettingsManager {
public:
    // Load settings from disk. Returns nullopt if file doesn't exist
    // or cannot be parsed.
    static std::optional<Settings> load();

    // Save settings to disk. Creates the directory if it doesn't exist.
    // Returns true on success.
    static bool save(const Settings& settings);

    // Returns the default file path for the settings file.
    static QString settingsFilePath();

    // Convert settings to/from JSON.
    static QJsonObject toJson(const Settings& settings);
    static Settings fromJson(const QJsonObject& json);
};

} // namespace bs
