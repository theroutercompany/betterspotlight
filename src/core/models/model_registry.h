#pragma once

#include "core/models/model_manifest.h"
#include "core/models/model_session.h"

#include <QString>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs {

class ModelRegistry {
public:
    explicit ModelRegistry(const QString& modelsDir);
    ~ModelRegistry();

    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;
    ModelRegistry(ModelRegistry&&) = delete;
    ModelRegistry& operator=(ModelRegistry&&) = delete;

    // Lazy-creates and caches a ModelSession for the given role (e.g. "bi-encoder").
    // Returns nullptr if the role is not in the manifest or initialization fails.
    ModelSession* getSession(const std::string& role);

    // Checks whether the manifest contains a model for the given role
    // without loading it.
    bool hasModel(const std::string& role) const;

    // Eagerly loads sessions for multiple roles.
    void preload(const std::vector<std::string>& roles);

    // Resolves the models directory by searching standard locations.
    // Search order:
    //   1. $BETTERSPOTLIGHT_MODELS_DIR environment variable
    //   2. App bundle Resources/models
    //   3. Build-dir relative paths
    //   4. $BETTERSPOTLIGHT_SOURCE_DIR/data/models
    // Looks for manifest.json to confirm a valid models directory.
    // Falls back to the first candidate if none contain the manifest.
    static QString resolveModelsDir();

    const ModelManifest& manifest() const;
    const QString& modelsDir() const;

private:
    QString m_modelsDir;
    ModelManifest m_manifest;
    std::unordered_map<std::string, std::unique_ptr<ModelSession>> m_sessions;
    mutable std::mutex m_mutex;
};

} // namespace bs
