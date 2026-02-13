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
    const QFileInfo destInfo(destPath);
    if (destInfo.exists() && destInfo.isReadable() && destInfo.size() > 0) {
        return true;
    }
    if (!QFileInfo::exists(sourcePath)) {
        return false;
    }
    QDir().mkpath(QFileInfo(destPath).absolutePath());
    QFile::remove(destPath);
    return QFile::copy(sourcePath, destPath);
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
    const QStringList candidates = modelDirCandidates(/*includeEnvOverride=*/true);

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

QString ModelRegistry::writableModelsDir()
{
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
        if (QFile::exists(candidate + QStringLiteral("/manifest.json"))) {
            sourceDir = candidate;
            break;
        }
    }
    if (sourceDir.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("No source models directory with manifest.json was found");
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

    // Seed bootstrap artifacts only; larger optional models are downloaded on demand.
    const QString vocabSrc = sourceDir + QStringLiteral("/vocab.txt");
    const QString vocabDst = destDir + QStringLiteral("/vocab.txt");
    if (!copyIfMissing(vocabSrc, vocabDst)) {
        LOG_WARN(bsCore, "ModelRegistry: vocab seed missing at %s", qPrintable(vocabSrc));
    }

    const QString smallSrc = sourceDir + QStringLiteral("/bge-small-en-v1.5-int8.onnx");
    const QString smallDst = destDir + QStringLiteral("/bge-small-en-v1.5-int8.onnx");
    if (!copyIfMissing(smallSrc, smallDst)) {
        LOG_WARN(bsCore, "ModelRegistry: bootstrap embedding model missing at %s",
                 qPrintable(smallSrc));
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
