#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"
#include "../Utils/model_fixture_paths.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

namespace {

bool prepareFastEmbeddingFixtureModelsDir(const QString& modelsDir)
{
    const QString sourceDir = bs::test::fixtureModelsSourceDir();
    if (sourceDir.isEmpty()) {
        return false;
    }

    QDir().mkpath(modelsDir);
    if (!bs::test::linkOrCopyFile(
            QDir(sourceDir).filePath(QStringLiteral("mxbai-embed-xsmall-v1-int8.onnx")),
            QDir(modelsDir).filePath(QStringLiteral("mxbai-embed-xsmall-v1-int8.onnx")))) {
        return false;
    }
    if (!bs::test::linkOrCopyFile(
            QDir(sourceDir).filePath(QStringLiteral("vocab.txt")),
            QDir(modelsDir).filePath(QStringLiteral("vocab.txt")))) {
        return false;
    }

    QFile manifestFile(QDir(modelsDir).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    manifestFile.write(R"({
        "models": {
            "bi-encoder-fast": {
                "name": "embed-fast-fixture",
                "modelId": "embed-fast-fixture-v1",
                "generationId": "v1",
                "file": "mxbai-embed-xsmall-v1-int8.onnx",
                "vocab": "vocab.txt",
                "dimensions": 384,
                "maxSeqLength": 512,
                "queryPrefix": "Represent this sentence: ",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask"],
                "outputs": ["last_hidden_state"],
                "extractionStrategy": "cls_token",
                "poolingStrategy": "cls_token",
                "semanticAggregationMode": "max_softmax_cap",
                "latencyTier": "fast",
                "task": "embedding",
                "providerPolicy": {
                    "preferredProvider": "cpu",
                    "preferCoreMl": false,
                    "allowCpuFallback": true
                }
            }
        }
    })");
    manifestFile.close();
    return true;
}

} // namespace

class TestEmbedding : public QObject {
    Q_OBJECT

private slots:
    void testConstructWithMissingModel();
    void testEmbedWithoutInit();
    void testQueryPrefixAdded();
    void testEmbedBatchWithoutModel();
    void testInitializeWithBadModel();
    void testNormalizeRejectsNonFiniteValues();
    void testFastTwoInputModelInitializesAndEmbeds();
};

void TestEmbedding::testConstructWithMissingModel()
{
    bs::ModelRegistry registry(QStringLiteral("/nonexistent/models"));
    bs::EmbeddingManager manager(&registry);
    QVERIFY(!manager.initialize());
    QVERIFY(!manager.isAvailable());
}

void TestEmbedding::testEmbedWithoutInit()
{
    bs::EmbeddingManager manager(nullptr);
    const std::vector<float> embedding = manager.embed(QStringLiteral("hello"));
    QVERIFY(embedding.empty());
}

void TestEmbedding::testQueryPrefixAdded()
{
    bs::EmbeddingManager manager(nullptr);

    const std::vector<float> queryEmbedding = manager.embedQuery(QStringLiteral("query text"));
    QVERIFY(queryEmbedding.empty());
    QVERIFY(!manager.isAvailable());
}

void TestEmbedding::testEmbedBatchWithoutModel()
{
    bs::EmbeddingManager manager(nullptr);

    std::vector<QString> texts = {
        QStringLiteral("hello"),
        QStringLiteral("world"),
        QStringLiteral("test"),
    };
    const std::vector<std::vector<float>> results = manager.embedBatch(texts);
    QVERIFY(results.empty());
}

void TestEmbedding::testInitializeWithBadModel()
{
    bs::ModelRegistry registry(QStringLiteral("/nonexistent/path"));
    bs::EmbeddingManager manager(&registry);
    bool ok = manager.initialize();
    QVERIFY(!ok);
    QVERIFY(!manager.isAvailable());

    const std::vector<float> embedding = manager.embed(QStringLiteral("test"));
    QVERIFY(embedding.empty());
}

void TestEmbedding::testNormalizeRejectsNonFiniteValues()
{
    using bs::embedding_detail::normalizeEmbeddingOrEmpty;

    const std::vector<float> normalized =
        normalizeEmbeddingOrEmpty({3.0F, 4.0F});
    QCOMPARE(static_cast<int>(normalized.size()), 2);
    QVERIFY(std::abs(normalized[0] - 0.6F) < 0.0001F);
    QVERIFY(std::abs(normalized[1] - 0.8F) < 0.0001F);

    const std::vector<float> zero =
        normalizeEmbeddingOrEmpty({0.0F, 0.0F, 0.0F});
    QCOMPARE(static_cast<int>(zero.size()), 3);
    QCOMPARE(zero[0], 0.0F);
    QCOMPARE(zero[1], 0.0F);
    QCOMPARE(zero[2], 0.0F);

    QVERIFY(normalizeEmbeddingOrEmpty({
        1.0F,
        std::numeric_limits<float>::quiet_NaN(),
    }).empty());
    QVERIFY(normalizeEmbeddingOrEmpty({
        1.0F,
        std::numeric_limits<float>::infinity(),
    }).empty());
    const std::vector<float> maxFinite = normalizeEmbeddingOrEmpty({
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    });
    QCOMPARE(static_cast<int>(maxFinite.size()), 2);
    QVERIFY(std::isfinite(maxFinite[0]));
    QVERIFY(std::isfinite(maxFinite[1]));
}

void TestEmbedding::testFastTwoInputModelInitializesAndEmbeds()
{
    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());
    QVERIFY2(prepareFastEmbeddingFixtureModelsDir(modelsDir.path()),
             "Failed to prepare two-input embedding fixture");

    const QByteArray oldDisableCoreMl = qgetenv("BETTERSPOTLIGHT_DISABLE_COREML");
    qputenv("BETTERSPOTLIGHT_DISABLE_COREML", QByteArrayLiteral("1"));
    const auto restoreEnv = qScopeGuard([&]() {
        if (oldDisableCoreMl.isNull()) {
            qunsetenv("BETTERSPOTLIGHT_DISABLE_COREML");
        } else {
            qputenv("BETTERSPOTLIGHT_DISABLE_COREML", oldDisableCoreMl);
        }
    });
    Q_UNUSED(restoreEnv);

    bs::ModelRegistry registry(modelsDir.path());
    bs::EmbeddingManager manager(&registry, "bi-encoder-fast");
    QVERIFY2(manager.initialize(), "Two-input embedding fixture should initialize");
    QVERIFY(manager.isAvailable());

    const std::vector<float> embedding = manager.embed(QStringLiteral("semantic embedding fixture"));
    QCOMPARE(static_cast<int>(embedding.size()), manager.embeddingDimensions());
}

QTEST_MAIN(TestEmbedding)
#include "test_embedding.moc"
