#pragma once

#include <QString>
#include <QJsonObject>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs {

struct ModelManifestEntry {
    QString name;
    QString file;
    QString vocab;
    int dimensions = 0;
    int maxSeqLength = 512;
    QString queryPrefix;
    QString tokenizer;
    std::vector<QString> inputs;
    std::vector<QString> outputs;
    QString extractionStrategy;
    QString outputTransform;
};

struct ModelManifest {
    std::unordered_map<std::string, ModelManifestEntry> models;

    static std::optional<ModelManifest> loadFromFile(const QString& path);
    static std::optional<ModelManifest> loadFromJson(const QJsonObject& root);
};

} // namespace bs
