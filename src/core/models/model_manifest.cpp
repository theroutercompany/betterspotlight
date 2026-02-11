#include "core/models/model_manifest.h"

#include "core/shared/logging.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace bs {

static std::optional<ModelManifestEntry> parseEntry(const QJsonObject& obj)
{
    if (!obj.contains(QStringLiteral("name")) || !obj.contains(QStringLiteral("file"))) {
        return std::nullopt;
    }

    ModelManifestEntry entry;
    entry.name = obj.value(QStringLiteral("name")).toString();
    entry.file = obj.value(QStringLiteral("file")).toString();
    entry.vocab = obj.value(QStringLiteral("vocab")).toString();
    entry.modelId = obj.value(QStringLiteral("modelId")).toString(entry.name);
    entry.generationId = obj.value(QStringLiteral("generationId")).toString(QStringLiteral("v1"));
    entry.fallbackRole = obj.value(QStringLiteral("fallbackRole")).toString();
    entry.dimensions = obj.value(QStringLiteral("dimensions")).toInt(0);
    entry.maxSeqLength = obj.value(QStringLiteral("maxSeqLength")).toInt(512);
    entry.queryPrefix = obj.value(QStringLiteral("queryPrefix")).toString();
    entry.tokenizer = obj.value(QStringLiteral("tokenizer")).toString();
    entry.extractionStrategy = obj.value(QStringLiteral("extractionStrategy")).toString();
    entry.poolingStrategy = obj.value(QStringLiteral("poolingStrategy"))
                                .toString(entry.extractionStrategy);
    entry.semanticAggregationMode = obj.value(QStringLiteral("semanticAggregationMode"))
                                        .toString(QStringLiteral("max_softmax_cap"));
    entry.outputTransform = obj.value(QStringLiteral("outputTransform")).toString();

    if (obj.contains(QStringLiteral("providerPolicy"))
        && obj.value(QStringLiteral("providerPolicy")).isObject()) {
        const QJsonObject providerPolicy = obj.value(QStringLiteral("providerPolicy")).toObject();
        entry.providerPolicy.preferredProvider =
            providerPolicy.value(QStringLiteral("preferredProvider"))
                .toString(QStringLiteral("coreml"));
        entry.providerPolicy.preferCoreMl =
            providerPolicy.value(QStringLiteral("preferCoreMl")).toBool(true);
        entry.providerPolicy.allowCpuFallback =
            providerPolicy.value(QStringLiteral("allowCpuFallback")).toBool(true);
        entry.providerPolicy.disableCoreMlEnvVar =
            providerPolicy.value(QStringLiteral("disableCoreMlEnvVar"))
                .toString(QStringLiteral("BETTERSPOTLIGHT_DISABLE_COREML"));
    }

    const QJsonArray inputsArray = obj.value(QStringLiteral("inputs")).toArray();
    entry.inputs.reserve(static_cast<size_t>(inputsArray.size()));
    for (const QJsonValue& v : inputsArray) {
        entry.inputs.push_back(v.toString());
    }

    const QJsonArray outputsArray = obj.value(QStringLiteral("outputs")).toArray();
    entry.outputs.reserve(static_cast<size_t>(outputsArray.size()));
    for (const QJsonValue& v : outputsArray) {
        entry.outputs.push_back(v.toString());
    }

    return entry;
}

std::optional<ModelManifest> ModelManifest::loadFromFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN(bsCore, "ModelManifest: cannot open %s", qPrintable(path));
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        LOG_WARN(bsCore, "ModelManifest: JSON parse error in %s: %s",
                 qPrintable(path), qPrintable(parseError.errorString()));
        return std::nullopt;
    }

    if (!doc.isObject()) {
        LOG_WARN(bsCore, "ModelManifest: root is not a JSON object in %s", qPrintable(path));
        return std::nullopt;
    }

    return loadFromJson(doc.object());
}

std::optional<ModelManifest> ModelManifest::loadFromJson(const QJsonObject& root)
{
    const QJsonValue modelsValue = root.value(QStringLiteral("models"));
    if (!modelsValue.isObject()) {
        LOG_WARN(bsCore, "ModelManifest: missing or invalid 'models' key");
        return std::nullopt;
    }

    const QJsonObject modelsObj = modelsValue.toObject();
    ModelManifest manifest;

    for (auto it = modelsObj.begin(); it != modelsObj.end(); ++it) {
        if (!it.value().isObject()) {
            LOG_WARN(bsCore, "ModelManifest: entry '%s' is not an object, skipping",
                     qPrintable(it.key()));
            continue;
        }

        std::optional<ModelManifestEntry> entry = parseEntry(it.value().toObject());
        if (!entry.has_value()) {
            LOG_WARN(bsCore, "ModelManifest: entry '%s' missing required fields, skipping",
                     qPrintable(it.key()));
            continue;
        }

        manifest.models[it.key().toStdString()] = std::move(entry.value());
    }

    return manifest;
}

} // namespace bs
