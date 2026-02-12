#include <QtTest/QtTest>

#include "core/models/model_manifest.h"
#include "core/models/model_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* key, const QByteArray& value)
        : key_(key)
        , oldValue_(qgetenv(key))
        , hadValue_(!oldValue_.isNull())
    {
        qputenv(key_, value);
    }

    ~ScopedEnvVar()
    {
        if (hadValue_) {
            qputenv(key_, oldValue_);
        } else {
            qunsetenv(key_);
        }
    }

private:
    const char* key_ = nullptr;
    QByteArray oldValue_;
    bool hadValue_ = false;
};

QString fixtureModelsSourceDir()
{
    const QString resolved = bs::ModelRegistry::resolveModelsDir();
    const QString modelPath =
        QDir(resolved).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    const QString vocabPath = QDir(resolved).filePath(QStringLiteral("vocab.txt"));
    if (QFileInfo::exists(modelPath) && QFileInfo::exists(vocabPath)) {
        return resolved;
    }
    return QStringLiteral("/Users/rexliu/betterspotlight/data/models");
}

bool linkOrCopyFile(const QString& sourcePath, const QString& destPath)
{
    QFile::remove(destPath);
    if (QFile::link(sourcePath, destPath)) {
        return true;
    }
    return QFile::copy(sourcePath, destPath);
}

bool prepareCrossEncoderFixtureDir(const QString& modelsDir, bool includeAliasFallbackRole)
{
    const QString sourceDir = fixtureModelsSourceDir();
    const QString sourceModel =
        QDir(sourceDir).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    const QString sourceVocab = QDir(sourceDir).filePath(QStringLiteral("vocab.txt"));
    if (!QFileInfo::exists(sourceModel) || !QFileInfo::exists(sourceVocab)) {
        return false;
    }

    if (!linkOrCopyFile(sourceModel,
                        QDir(modelsDir).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx")))) {
        return false;
    }
    if (!linkOrCopyFile(sourceVocab,
                        QDir(modelsDir).filePath(QStringLiteral("vocab.txt")))) {
        return false;
    }

    QJsonObject crossEntry;
    crossEntry.insert(QStringLiteral("name"), QStringLiteral("cross-fixture"));
    crossEntry.insert(QStringLiteral("modelId"), QStringLiteral("cross-fixture-v1"));
    crossEntry.insert(QStringLiteral("generationId"), QStringLiteral("v1"));
    crossEntry.insert(QStringLiteral("file"), QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    crossEntry.insert(QStringLiteral("vocab"), QStringLiteral("vocab.txt"));
    crossEntry.insert(QStringLiteral("tokenizer"), QStringLiteral("wordpiece"));
    crossEntry.insert(QStringLiteral("task"), QStringLiteral("rerank"));
    crossEntry.insert(QStringLiteral("inputs"), QJsonArray{
        QStringLiteral("input_ids"),
        QStringLiteral("attention_mask"),
        QStringLiteral("token_type_ids"),
    });
    crossEntry.insert(QStringLiteral("outputs"), QJsonArray{
        QStringLiteral("logits"),
    });

    QJsonObject models;
    models.insert(QStringLiteral("cross-encoder"), crossEntry);
    if (includeAliasFallbackRole) {
        QJsonObject aliasEntry = crossEntry;
        aliasEntry.insert(QStringLiteral("file"), QStringLiteral("missing-model.onnx"));
        aliasEntry.insert(QStringLiteral("fallbackRole"), QStringLiteral("cross-encoder"));
        models.insert(QStringLiteral("alias-role"), aliasEntry);
    }

    QJsonObject root;
    root.insert(QStringLiteral("models"), models);

    QFile manifestFile(QDir(modelsDir).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    manifestFile.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    manifestFile.close();
    return true;
}

} // namespace

class TestModelRegistry : public QObject {
    Q_OBJECT

private slots:
    void testManifestParsing();
    void testManifestMissingFile();
    void testManifestInvalidJson();
    void testRegistryGetSessionUnknownRole();

private:
    void runResolveModelsDirUsesEnvOverride();
    void runGetSessionFallbackRoleAndPreload();
    void runGetSessionFallbackCycleStops();
    void runEnsureWritableModelsSeeded();
};

void TestModelRegistry::testManifestParsing()
{
    const QByteArray json = R"({
        "models": {
            "bi-encoder": {
                "name": "bge-small-en-v1.5",
                "modelId": "bge-small-en-v1.5-int8",
                "generationId": "v1",
                "file": "bge-small-en-v1.5-int8.onnx",
                "vocab": "vocab.txt",
                "dimensions": 384,
                "maxSeqLength": 512,
                "queryPrefix": "Represent this sentence: ",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask", "token_type_ids"],
                "outputs": ["last_hidden_state"],
                "extractionStrategy": "cls_token",
                "semanticAggregationMode": "max_softmax_cap",
                "latencyTier": "strong",
                "task": "embedding",
                "providerPolicy": {
                    "preferredProvider": "coreml",
                    "preferCoreMl": true,
                    "allowCpuFallback": true
                }
            },
            "cross-encoder": {
                "name": "ms-marco-MiniLM-L-6-v2",
                "modelId": "ms-marco-mini",
                "generationId": "v2",
                "file": "ms-marco-minilm.onnx",
                "vocab": "vocab.txt",
                "dimensions": 1,
                "maxSeqLength": 512,
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask"],
                "outputs": ["logits"],
                "extractionStrategy": "single_score",
                "latencyTier": "fast",
                "task": "rerank"
            }
        }
    })";

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);

    const std::optional<bs::ModelManifest> manifest = bs::ModelManifest::loadFromJson(doc.object());
    QVERIFY(manifest.has_value());
    QCOMPARE(manifest->models.size(), 2UL);

    // Verify bi-encoder entry
    auto biIt = manifest->models.find("bi-encoder");
    QVERIFY(biIt != manifest->models.end());
    const bs::ModelManifestEntry& biEncoder = biIt->second;
    QCOMPARE(biEncoder.name, QStringLiteral("bge-small-en-v1.5"));
    QCOMPARE(biEncoder.file, QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    QCOMPARE(biEncoder.vocab, QStringLiteral("vocab.txt"));
    QCOMPARE(biEncoder.modelId, QStringLiteral("bge-small-en-v1.5-int8"));
    QCOMPARE(biEncoder.generationId, QStringLiteral("v1"));
    QCOMPARE(biEncoder.dimensions, 384);
    QCOMPARE(biEncoder.maxSeqLength, 512);
    QCOMPARE(biEncoder.tokenizer, QStringLiteral("wordpiece"));
    QCOMPARE(biEncoder.extractionStrategy, QStringLiteral("cls_token"));
    QCOMPARE(biEncoder.inputs.size(), 3UL);
    QCOMPARE(biEncoder.inputs[0], QStringLiteral("input_ids"));
    QCOMPARE(biEncoder.inputs[1], QStringLiteral("attention_mask"));
    QCOMPARE(biEncoder.inputs[2], QStringLiteral("token_type_ids"));
    QCOMPARE(biEncoder.outputs.size(), 1UL);
    QCOMPARE(biEncoder.outputs[0], QStringLiteral("last_hidden_state"));
    QCOMPARE(biEncoder.latencyTier, QStringLiteral("strong"));
    QCOMPARE(biEncoder.task, QStringLiteral("embedding"));
    QCOMPARE(biEncoder.providerPolicy.preferredProvider, QStringLiteral("coreml"));
    QCOMPARE(biEncoder.providerPolicy.preferCoreMl, true);
    QCOMPARE(biEncoder.providerPolicy.allowCpuFallback, true);

    // Verify cross-encoder entry
    auto ceIt = manifest->models.find("cross-encoder");
    QVERIFY(ceIt != manifest->models.end());
    const bs::ModelManifestEntry& crossEncoder = ceIt->second;
    QCOMPARE(crossEncoder.name, QStringLiteral("ms-marco-MiniLM-L-6-v2"));
    QCOMPARE(crossEncoder.dimensions, 1);
    QCOMPARE(crossEncoder.inputs.size(), 2UL);
    QCOMPARE(crossEncoder.latencyTier, QStringLiteral("fast"));
    QCOMPARE(crossEncoder.task, QStringLiteral("rerank"));
}

void TestModelRegistry::testManifestMissingFile()
{
    const std::optional<bs::ModelManifest> manifest =
        bs::ModelManifest::loadFromFile(QStringLiteral("/nonexistent/path/manifest.json"));
    QVERIFY(!manifest.has_value());
}

void TestModelRegistry::testManifestInvalidJson()
{
    QTemporaryFile tempFile;
    QVERIFY(tempFile.open());
    tempFile.write("{ this is not valid json }}}");
    tempFile.close();

    const std::optional<bs::ModelManifest> manifest =
        bs::ModelManifest::loadFromFile(tempFile.fileName());
    QVERIFY(!manifest.has_value());
}

void TestModelRegistry::testRegistryGetSessionUnknownRole()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Write a minimal manifest
    QFile manifestFile(tempDir.path() + QStringLiteral("/manifest.json"));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(R"({"models":{"bi-encoder":{"name":"test","file":"test.onnx","vocab":"v.txt","dimensions":384}}})");
    manifestFile.close();

    bs::ModelRegistry registry(tempDir.path());

    // Requesting an unknown role should return nullptr
    bs::ModelSession* session = registry.getSession("unknown-role");
    QVERIFY(session == nullptr);

    // Verify hasModel works
    QVERIFY(registry.hasModel("bi-encoder"));
    QVERIFY(!registry.hasModel("unknown-role"));

    // Execute additional registry behavior scenarios from an existing slot to
    // avoid stale Qt moc slot metadata in incremental test builds.
    runResolveModelsDirUsesEnvOverride();
    runGetSessionFallbackRoleAndPreload();
    runGetSessionFallbackCycleStops();
    runEnsureWritableModelsSeeded();
}

void TestModelRegistry::runResolveModelsDirUsesEnvOverride()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QFile manifestFile(tempDir.path() + QStringLiteral("/manifest.json"));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifestFile.write("{\"models\":{}}");
    manifestFile.close();

    ScopedEnvVar envModelsDir("BETTERSPOTLIGHT_MODELS_DIR", tempDir.path().toUtf8());
    const QString resolved = bs::ModelRegistry::resolveModelsDir();
    QCOMPARE(QDir::cleanPath(resolved), QDir::cleanPath(tempDir.path()));
}

void TestModelRegistry::runGetSessionFallbackRoleAndPreload()
{
    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());
    QVERIFY2(prepareCrossEncoderFixtureDir(modelsDir.path(), /*includeAliasFallbackRole=*/true),
             "Failed to prepare fixture models directory");

    ScopedEnvVar disableCoreMl("BETTERSPOTLIGHT_DISABLE_COREML", QByteArrayLiteral("1"));

    bs::ModelRegistry registry(modelsDir.path());
    QVERIFY(registry.hasModel("cross-encoder"));
    QVERIFY(registry.hasModel("alias-role"));

    bs::ModelSession* aliasSession = registry.getSession("alias-role");
    QVERIFY(aliasSession != nullptr);

    bs::ModelSession* directSession = registry.getSession("cross-encoder");
    QVERIFY(directSession != nullptr);
    QCOMPARE(aliasSession, directSession);

    registry.preload({"cross-encoder", "alias-role", "missing-role"});
    QCOMPARE(registry.getSession("cross-encoder"), directSession);

    QCOMPARE(QDir::cleanPath(registry.modelsDir()), QDir::cleanPath(modelsDir.path()));
    QVERIFY(registry.manifest().models.find("cross-encoder") != registry.manifest().models.end());
}

void TestModelRegistry::runGetSessionFallbackCycleStops()
{
    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());

    QJsonObject cycleA;
    cycleA.insert(QStringLiteral("name"), QStringLiteral("cycle-a"));
    cycleA.insert(QStringLiteral("file"), QStringLiteral("missing-a.onnx"));
    cycleA.insert(QStringLiteral("fallbackRole"), QStringLiteral("role-b"));

    QJsonObject cycleB;
    cycleB.insert(QStringLiteral("name"), QStringLiteral("cycle-b"));
    cycleB.insert(QStringLiteral("file"), QStringLiteral("missing-b.onnx"));
    cycleB.insert(QStringLiteral("fallbackRole"), QStringLiteral("role-a"));

    QJsonObject models;
    models.insert(QStringLiteral("role-a"), cycleA);
    models.insert(QStringLiteral("role-b"), cycleB);

    QJsonObject root;
    root.insert(QStringLiteral("models"), models);

    QFile manifestFile(modelsDir.path() + QStringLiteral("/manifest.json"));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifestFile.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    manifestFile.close();

    bs::ModelRegistry registry(modelsDir.path());
    QElapsedTimer timer;
    timer.start();
    bs::ModelSession* session = registry.getSession("role-a");
    QVERIFY(session == nullptr);
    QVERIFY(timer.elapsed() < 2000);
}

void TestModelRegistry::runEnsureWritableModelsSeeded()
{
    QStandardPaths::setTestModeEnabled(true);

    const QString sourceDir = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/models"));
    QVERIFY(QDir().mkpath(sourceDir));

    QFile sourceManifest(sourceDir + QStringLiteral("/manifest.json"));
    QVERIFY(sourceManifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
    sourceManifest.write("{\"models\":{}}");
    sourceManifest.close();

    QString error;
    QVERIFY(bs::ModelRegistry::ensureWritableModelsSeeded(&error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QString writable = bs::ModelRegistry::writableModelsDir();
    const QString manifestPath = writable + QStringLiteral("/manifest.json");
    const QString vocabPath = writable + QStringLiteral("/vocab.txt");
    QVERIFY(QFileInfo::exists(manifestPath));
    QVERIFY(QFileInfo(manifestPath).size() > 0);

    // Force reseed path by truncating a required file to zero bytes.
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifestFile.close();
    QCOMPARE(QFileInfo(manifestPath).size(), 0LL);

    QVERIFY(bs::ModelRegistry::ensureWritableModelsSeeded(&error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(QFileInfo(manifestPath).size() > 0);

    // Optional artifacts may be absent on some hosts; when present they should
    // never be zero-sized after seeding.
    if (QFileInfo::exists(vocabPath)) {
        QVERIFY(QFileInfo(vocabPath).size() > 0);
    }
}

QTEST_MAIN(TestModelRegistry)
#include "test_model_registry.moc"
