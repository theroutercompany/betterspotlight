#pragma once

#include <QString>
#include <QJsonObject>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs {

struct ModelProviderPolicy {
    QString preferredProvider = QStringLiteral("coreml");
    bool preferCoreMl = true;
    bool allowCpuFallback = true;
    QString disableCoreMlEnvVar = QStringLiteral("BETTERSPOTLIGHT_DISABLE_COREML");
};

struct ModelManifestEntry {
    QString name;
    QString file;
    QString vocab;
    QString modelId;
    QString generationId = QStringLiteral("v1");
    QString fallbackRole;
    int dimensions = 0;
    int maxSeqLength = 512;
    QString queryPrefix;
    QString tokenizer;
    std::vector<QString> inputs;
    std::vector<QString> outputs;
    QString extractionStrategy;
    QString poolingStrategy;
    QString semanticAggregationMode = QStringLiteral("max_softmax_cap");
    QString outputTransform;
    QString latencyTier = QStringLiteral("balanced");
    QString task;
    ModelProviderPolicy providerPolicy;
};

struct ModelManifest {
    std::unordered_map<std::string, ModelManifestEntry> models;

    static std::optional<ModelManifest> loadFromFile(const QString& path);
    static std::optional<ModelManifest> loadFromJson(const QJsonObject& root);
};

} // namespace bs
