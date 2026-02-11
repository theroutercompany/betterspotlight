#include "core/models/model_registry.h"

#include "core/shared/logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>

namespace bs {

ModelRegistry::ModelRegistry(const QString& modelsDir)
    : m_modelsDir(modelsDir)
{
    const QString manifestPath = m_modelsDir + QStringLiteral("/manifest.json");
    std::optional<ModelManifest> loaded = ModelManifest::loadFromFile(manifestPath);
    if (loaded.has_value()) {
        m_manifest = std::move(loaded.value());
        LOG_INFO(bsCore, "ModelRegistry: loaded manifest with %zu model(s) from %s",
                 m_manifest.models.size(), qPrintable(manifestPath));
    } else {
        LOG_WARN(bsCore, "ModelRegistry: failed to load manifest from %s", qPrintable(manifestPath));
    }
}

ModelRegistry::~ModelRegistry() = default;

ModelSession* ModelRegistry::getSession(const std::string& role)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_set<std::string> visited;
    visited.insert(role);
    return getSessionUnlocked(role, visited);
}

ModelSession* ModelRegistry::getSessionUnlocked(const std::string& role,
                                                std::unordered_set<std::string>& visited)
{
    auto sessionIt = m_sessions.find(role);
    if (sessionIt != m_sessions.end()) {
        return sessionIt->second.get();
    }

    auto manifestIt = m_manifest.models.find(role);
    if (manifestIt == m_manifest.models.end()) {
        LOG_WARN(bsCore, "ModelRegistry: no manifest entry for role '%s'", role.c_str());
        return nullptr;
    }

    const ModelManifestEntry& entry = manifestIt->second;
    const QString modelPath = m_modelsDir + QStringLiteral("/") + entry.file;

    auto session = std::make_unique<ModelSession>(entry);
    if (!session->initialize(modelPath)) {
        if (!entry.fallbackRole.isEmpty()) {
            const std::string fallbackRole = entry.fallbackRole.toStdString();
            if (!visited.count(fallbackRole)) {
                visited.insert(fallbackRole);
                LOG_WARN(bsCore,
                         "ModelRegistry: failed to initialize role '%s', trying fallback role '%s'",
                         role.c_str(), fallbackRole.c_str());
                return getSessionUnlocked(fallbackRole, visited);
            }
        }
        LOG_WARN(bsCore, "ModelRegistry: failed to initialize session for role '%s'",
                 role.c_str());
        return nullptr;
    }

    ModelSession* raw = session.get();
    m_sessions[role] = std::move(session);
    return raw;
}

bool ModelRegistry::hasModel(const std::string& role) const
{
    return m_manifest.models.find(role) != m_manifest.models.end();
}

void ModelRegistry::preload(const std::vector<std::string>& roles)
{
    for (const std::string& role : roles) {
        getSession(role);
    }
}

QString ModelRegistry::resolveModelsDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();

    QStringList candidates;

    const QString envModelDir =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"));
    if (!envModelDir.isEmpty()) {
        candidates << QDir::cleanPath(envModelDir);
    }

    candidates << QDir::cleanPath(appDir + QStringLiteral("/../Resources/models"));
    candidates << QDir::cleanPath(appDir + QStringLiteral("/../../app/betterspotlight.app/Contents/Resources/models"));
    candidates << QDir::cleanPath(appDir + QStringLiteral("/../../../app/betterspotlight.app/Contents/Resources/models"));
    candidates << QDir::cleanPath(appDir + QStringLiteral("/../../../../data/models"));

#ifdef BETTERSPOTLIGHT_SOURCE_DIR
    candidates << QDir::cleanPath(QString::fromUtf8(BETTERSPOTLIGHT_SOURCE_DIR)
                                  + QStringLiteral("/data/models"));
#endif

    candidates.removeDuplicates();

    for (const QString& dir : candidates) {
        const QString manifestPath = dir + QStringLiteral("/manifest.json");
        if (QFile::exists(manifestPath)) {
            LOG_INFO(bsCore, "ModelRegistry: resolved models dir to %s", qPrintable(dir));
            return dir;
        }
    }

    LOG_WARN(bsCore, "ModelRegistry: manifest.json not found in any candidate dir. Searched: %s",
             qPrintable(candidates.join(QStringLiteral(", "))));

    const QString fallback = candidates.isEmpty()
        ? QDir::cleanPath(appDir + QStringLiteral("/../Resources/models"))
        : candidates.first();
    return fallback;
}

const ModelManifest& ModelRegistry::manifest() const
{
    return m_manifest;
}

const QString& ModelRegistry::modelsDir() const
{
    return m_modelsDir;
}

} // namespace bs
