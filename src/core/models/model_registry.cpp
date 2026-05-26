#include "core/models/model_registry.h"

#include "core/shared/logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace bs {

namespace {

bool manifestHasRequiredRoles(const ModelManifest& manifest,
                              QStringList* missingRolesOut = nullptr)
{
    QStringList missingRoles;
    for (const QString& role : ModelRegistry::requiredProductionRoles()) {
        const auto it = manifest.models.find(role.toStdString());
        if (it == manifest.models.end() || it->second.file.trimmed().isEmpty()) {
            missingRoles.append(role);
        }
    }
    if (missingRolesOut) {
        *missingRolesOut = missingRoles;
    }
    return missingRoles.isEmpty();
}

std::optional<ModelManifest> loadUsableManifest(const QString& dir,
                                                QString* reasonOut = nullptr)
{
    if (reasonOut) {
        *reasonOut = QString();
    }

    const QString manifestPath = QDir(dir).filePath(QStringLiteral("manifest.json"));
    if (!QFile::exists(manifestPath)) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("manifest_missing");
        }
        return std::nullopt;
    }

    const std::optional<ModelManifest> manifest = ModelManifest::loadFromFile(manifestPath);
    if (!manifest.has_value()) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("manifest_invalid");
        }
        return std::nullopt;
    }

    QStringList missingRoles;
    if (!manifestHasRequiredRoles(manifest.value(), &missingRoles)) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("missing_required_roles:%1")
                             .arg(missingRoles.join(QStringLiteral(",")));
        }
        return std::nullopt;
    }

    return manifest;
}

QStringList modelDirCandidates(bool includeEnvOverride)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;

    if (includeEnvOverride) {
        const QString envModelDir =
            QProcessEnvironment::systemEnvironment().value(
                QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"));
        if (!envModelDir.isEmpty()) {
            candidates << QDir::cleanPath(envModelDir);
        }
    }

    candidates << QDir::cleanPath(appDir + QStringLiteral("/../Resources/models"));
    candidates << QDir::cleanPath(
        appDir + QStringLiteral("/../../app/betterspotlight.app/Contents/Resources/models"));
    candidates << QDir::cleanPath(
        appDir + QStringLiteral("/../../../app/betterspotlight.app/Contents/Resources/models"));
    candidates << QDir::cleanPath(appDir + QStringLiteral("/../../../../data/models"));

#ifdef BETTERSPOTLIGHT_SOURCE_DIR
    candidates << QDir::cleanPath(QString::fromUtf8(BETTERSPOTLIGHT_SOURCE_DIR)
                                  + QStringLiteral("/data/models"));
#endif

    candidates.removeDuplicates();
    return candidates;
}

bool copyIfMissing(const QString& sourcePath, const QString& destPath)
{
    auto ensureOwnerWritable = [](const QString& path) {
        QFile file(path);
        const QFileDevice::Permissions permissions = file.permissions();
        file.setPermissions(permissions | QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    };

    const QFileInfo destInfo(destPath);
    if (destInfo.exists() && destInfo.isReadable() && destInfo.size() > 0) {
        ensureOwnerWritable(destPath);
        return true;
    }
    if (!QFileInfo::exists(sourcePath)) {
        return false;
    }
    QDir().mkpath(QFileInfo(destPath).absolutePath());
    QFile::remove(destPath);
    if (!QFile::copy(sourcePath, destPath)) {
        return false;
    }
    ensureOwnerWritable(destPath);
    return true;
}

bool copyDirectoryIfMissing(const QString& sourcePath, const QString& destPath)
{
    if (QFileInfo(destPath).isDir() && QDir(destPath).exists()) {
        const QDir existing(destPath);
        if (!existing.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System)
                 .isEmpty()) {
            return true;
        }
    }

    const QDir sourceDir(sourcePath);
    if (!sourceDir.exists()) {
        return false;
    }

    if (!QDir().mkpath(destPath)) {
        return false;
    }

    const QFileInfoList entries = sourceDir.entryInfoList(
        QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        const QString destinationEntry = QDir(destPath).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectoryIfMissing(entry.filePath(), destinationEntry)) {
                return false;
            }
            continue;
        }
        if (!copyIfMissing(entry.filePath(), destinationEntry)) {
            return false;
        }
    }

    return true;
}

} // namespace

ModelRegistry::ModelRegistry(const QString& modelsDir)
    : m_modelsDir(modelsDir)
{
    const QString manifestPath = m_modelsDir + QStringLiteral("/manifest.json");
    std::optional<ModelManifest> loaded = ModelManifest::loadFromFile(manifestPath);
    if (loaded.has_value()) {
        m_manifest = std::move(loaded.value());
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
    const QStringList candidates = modelDirCandidates(/*includeEnvOverride=*/true);

    for (const QString& dir : candidates) {
        QString rejectionReason;
        if (loadUsableManifest(dir, &rejectionReason).has_value()) {
            LOG_INFO(bsCore, "ModelRegistry: resolved models dir to %s", qPrintable(dir));
            return dir;
        }
        if (!rejectionReason.isEmpty()) {
            LOG_WARN(bsCore,
                     "ModelRegistry: rejecting models dir %s (%s)",
                     qPrintable(dir),
                     qPrintable(rejectionReason));
        }
    }

    LOG_ERROR(bsCore, "ModelRegistry: no usable models directory found. Searched: %s",
             qPrintable(candidates.join(QStringLiteral(", "))));
    return QString();
}

QStringList ModelRegistry::requiredProductionRoles()
{
    return {
        QStringLiteral("bi-encoder"),
        QStringLiteral("cross-encoder"),
    };
}

const ModelManifest& ModelRegistry::manifest() const
{
    return m_manifest;
}

const QString& ModelRegistry::modelsDir() const
{
    return m_modelsDir;
}

bool ModelRegistry::hasRequiredProductionRoles(QStringList* missingRolesOut) const
{
    return manifestHasRequiredRoles(m_manifest, missingRolesOut);
}

QString ModelRegistry::writableModelsDir()
{
    const QString dataDir = QProcessEnvironment::systemEnvironment()
                                .value(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR"))
                                .trimmed();
    if (!dataDir.isEmpty()) {
        return QDir::cleanPath(dataDir + QStringLiteral("/models"));
    }

    return QDir::cleanPath(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/models"));
}

bool ModelRegistry::ensureWritableModelsSeeded(QString* errorOut)
{
    if (errorOut) {
        *errorOut = QString();
    }

    const QString destDir = writableModelsDir();
    if (!QDir().mkpath(destDir)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to create writable model directory: %1")
                            .arg(destDir);
        }
        return false;
    }

    // Use non-env candidates to avoid self-copying if caller already exported
    // BETTERSPOTLIGHT_MODELS_DIR.
    const QStringList sources = modelDirCandidates(/*includeEnvOverride=*/false);
    QString sourceDir;
    for (const QString& candidate : sources) {
        if (loadUsableManifest(candidate).has_value()) {
            sourceDir = candidate;
            break;
        }
    }
    if (sourceDir.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("No usable source models directory was found");
        }
        return false;
    }

    const QString manifestSrc = sourceDir + QStringLiteral("/manifest.json");
    const QString manifestDst = destDir + QStringLiteral("/manifest.json");
    if (!copyIfMissing(manifestSrc, manifestDst)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to seed manifest.json into writable models dir");
        }
        return false;
    }

    const std::optional<ModelManifest> manifest = ModelManifest::loadFromFile(manifestDst);
    if (!manifest.has_value()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to load seeded manifest.json from writable models dir");
        }
        return false;
    }
    if (!manifestHasRequiredRoles(manifest.value())) {
        if (errorOut) {
            *errorOut = QStringLiteral("Seeded manifest.json is missing required production roles");
        }
        return false;
    }

    for (const auto& [role, entry] : manifest->models) {
        if (entry.file.trimmed().isEmpty()) {
            continue;
        }
        const QString modelSrc = sourceDir + QStringLiteral("/") + entry.file;
        const QString modelDst = destDir + QStringLiteral("/") + entry.file;
        if (!copyIfMissing(modelSrc, modelDst)) {
            LOG_INFO(bsCore,
                     "ModelRegistry: runtime model for role '%s' not seeded from %s (download may still be required)",
                     role.c_str(),
                     qPrintable(modelSrc));
        }
    }

    const QString vocabSrc = sourceDir + QStringLiteral("/vocab.txt");
    const QString vocabDst = destDir + QStringLiteral("/vocab.txt");
    if (!copyIfMissing(vocabSrc, vocabDst)) {
        LOG_WARN(bsCore, "ModelRegistry: vocab seed missing at %s", qPrintable(vocabSrc));
    }

    const QString onlineRankerSrc =
        sourceDir + QStringLiteral("/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc");
    const QString onlineRankerDst =
        destDir + QStringLiteral("/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc");
    if (!copyDirectoryIfMissing(onlineRankerSrc, onlineRankerDst)) {
        LOG_INFO(bsCore,
                 "ModelRegistry: online ranker bootstrap model not present at %s (optional)",
                 qPrintable(onlineRankerSrc));
    }
    const QString onlineRankerMetadataSrc =
        sourceDir + QStringLiteral("/online-ranker-v1/bootstrap/metadata.json");
    const QString onlineRankerMetadataDst =
        destDir + QStringLiteral("/online-ranker-v1/bootstrap/metadata.json");
    if (!copyIfMissing(onlineRankerMetadataSrc, onlineRankerMetadataDst)) {
        LOG_INFO(bsCore,
                 "ModelRegistry: online ranker bootstrap metadata not present at %s (optional)",
                 qPrintable(onlineRankerMetadataSrc));
    }

    LOG_INFO(bsCore, "ModelRegistry: writable model cache ready at %s",
             qPrintable(destDir));
    return true;
}

} // namespace bs
