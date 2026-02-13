#include "model_fixture_paths.h"

#include "core/models/model_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace bs::test {
namespace {

bool hasFixtureModelPair(const QString& dirPath)
{
    const QString model = QDir(dirPath).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    const QString vocab = QDir(dirPath).filePath(QStringLiteral("vocab.txt"));
    return QFileInfo::exists(model) && QFileInfo::exists(vocab);
}

QString findRepoModelsDirFrom(const QString& start)
{
    if (start.isEmpty()) {
        return QString();
    }

    QDir cursor(start);
    for (int depth = 0; depth < 14; ++depth) {
        const QString candidate = QDir::cleanPath(cursor.filePath(QStringLiteral("data/models")));
        if (hasFixtureModelPair(candidate)) {
            return candidate;
        }
        if (!cursor.cdUp()) {
            break;
        }
    }
    return QString();
}

} // namespace

QString fixtureModelsSourceDir()
{
    const QString explicitEnv = QString::fromUtf8(qgetenv("BETTERSPOTLIGHT_TEST_MODELS_DIR"));
    if (!explicitEnv.isEmpty() && hasFixtureModelPair(explicitEnv)) {
        return QDir::cleanPath(explicitEnv);
    }

    const QString resolved = bs::ModelRegistry::resolveModelsDir();
    if (hasFixtureModelPair(resolved)) {
        return QDir::cleanPath(resolved);
    }

    const QString appCandidate =
        findRepoModelsDirFrom(QCoreApplication::applicationDirPath());
    if (!appCandidate.isEmpty()) {
        return appCandidate;
    }

    const QString cwdCandidate = findRepoModelsDirFrom(QDir::currentPath());
    if (!cwdCandidate.isEmpty()) {
        return cwdCandidate;
    }

    return QString();
}

bool linkOrCopyFile(const QString& sourcePath, const QString& targetPath)
{
    QFile::remove(targetPath);
    if (QFile::link(sourcePath, targetPath)) {
        return true;
    }
    return QFile::copy(sourcePath, targetPath);
}

bool prepareFixtureEmbeddingModelFiles(const QString& modelsDir)
{
    const QString sourceDir = fixtureModelsSourceDir();
    if (sourceDir.isEmpty()) {
        return false;
    }

    const QString sourceModel =
        QDir(sourceDir).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    const QString sourceVocab = QDir(sourceDir).filePath(QStringLiteral("vocab.txt"));

    if (!QFileInfo::exists(sourceModel) || !QFileInfo::exists(sourceVocab)) {
        return false;
    }

    QDir().mkpath(modelsDir);
    if (!linkOrCopyFile(sourceModel,
                        QDir(modelsDir).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx")))) {
        return false;
    }
    if (!linkOrCopyFile(sourceVocab,
                        QDir(modelsDir).filePath(QStringLiteral("vocab.txt")))) {
        return false;
    }

    return true;
}

} // namespace bs::test
