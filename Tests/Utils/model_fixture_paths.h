#pragma once

#include <QString>

namespace bs::test {

QString fixtureModelsSourceDir();
bool linkOrCopyFile(const QString& sourcePath, const QString& targetPath);
bool prepareFixtureEmbeddingModelFiles(const QString& modelsDir);

} // namespace bs::test
