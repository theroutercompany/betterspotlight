#include <QtTest/QtTest>
#include "core/ranking/cross_encoder_reranker.h"
#include "core/models/model_registry.h"
#include "core/shared/search_result.h"
#include "../Utils/model_fixture_paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>

namespace {

bool prepareCrossEncoderFixtureModelsDir(const QString& modelsDir)
{
    if (!bs::test::prepareFixtureEmbeddingModelFiles(modelsDir)) {
        return false;
    }

    const QByteArray manifest = R"({
        "models": {
            "cross-encoder": {
                "name": "cross-fixture",
                "modelId": "cross-fixture-v1",
                "generationId": "v1",
                "file": "bge-small-en-v1.5-int8.onnx",
                "vocab": "vocab.txt",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask", "token_type_ids"],
                "outputs": ["logits"],
                "task": "rerank"
            }
        }
    })";
    QFile manifestFile(QDir(modelsDir).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    manifestFile.write(manifest);
    manifestFile.close();
    return true;
}

} // namespace

class TestCrossEncoderReranker : public QObject {
    Q_OBJECT

private slots:
    void testConstructWithoutModel();
    void testRerankWithUnavailableModel();
    void testMaxCandidatesCapping();
};

void TestCrossEncoderReranker::testConstructWithoutModel()
{
    // No registry at all — initialize should fail gracefully
    bs::CrossEncoderReranker reranker(nullptr);
    QVERIFY(!reranker.initialize());
    QVERIFY(!reranker.isAvailable());
}

void TestCrossEncoderReranker::testRerankWithUnavailableModel()
{
    bs::CrossEncoderReranker reranker(nullptr);
    QVERIFY(!reranker.initialize());

    // Create some results
    std::vector<bs::SearchResult> results;
    for (int i = 0; i < 5; ++i) {
        bs::SearchResult sr;
        sr.itemId = i + 1;
        sr.path = QStringLiteral("/home/user/doc_%1.pdf").arg(i);
        sr.name = QStringLiteral("doc_%1.pdf").arg(i);
        sr.score = 100.0 - i * 10.0;
        results.push_back(sr);
    }

    // Capture original scores
    std::vector<double> originalScores;
    for (const auto& r : results) {
        originalScores.push_back(r.score);
    }

    // rerank should return 0 and leave results unchanged
    const int boosted = reranker.rerank(QStringLiteral("test query"), results);
    QCOMPARE(boosted, 0);

    for (size_t i = 0; i < results.size(); ++i) {
        QCOMPARE(results[i].score, originalScores[i]);
        QCOMPARE(results[i].scoreBreakdown.crossEncoderBoost, 0.0);
        QCOMPARE(results[i].crossEncoderScore, 0.0f);
    }
}

void TestCrossEncoderReranker::testMaxCandidatesCapping()
{
    bs::CrossEncoderReranker unavailableReranker(nullptr);
    // Don't initialize — model unavailable

    std::vector<bs::SearchResult> unavailableResults;
    for (int i = 0; i < 100; ++i) {
        bs::SearchResult sr;
        sr.itemId = i + 1;
        sr.path = QStringLiteral("/home/user/file_%1.txt").arg(i);
        sr.name = QStringLiteral("file_%1.txt").arg(i);
        sr.score = 200.0 - i;
        unavailableResults.push_back(sr);
    }

    bs::RerankerConfig unavailableConfig;
    unavailableConfig.maxCandidates = 10;

    // With unavailable model, rerank returns 0 regardless of config
    const int unavailableBoosted = unavailableReranker.rerank(
        QStringLiteral("query"), unavailableResults, unavailableConfig);
    QCOMPARE(unavailableBoosted, 0);
    QCOMPARE(static_cast<int>(unavailableResults.size()), 100); // results not truncated

    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());
    QVERIFY2(prepareCrossEncoderFixtureModelsDir(modelsDir.path()),
             "Failed to prepare fixture models directory for cross-encoder");

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
    bs::CrossEncoderReranker reranker(&registry, "cross-encoder");
    QVERIFY2(reranker.initialize(), "Cross-encoder fixture should initialize");
    QVERIFY(reranker.isAvailable());

    std::vector<bs::SearchResult> results;
    results.reserve(3);
    for (int i = 0; i < 3; ++i) {
        bs::SearchResult sr;
        sr.itemId = i + 1;
        sr.path = QStringLiteral("/tmp/doc_%1.md").arg(i + 1);
        sr.name = QStringLiteral("doc_%1.md").arg(i + 1);
        sr.snippet = QStringLiteral("semantic rerank fixture snippet %1").arg(i + 1);
        sr.score = 10.0 - static_cast<double>(i);
        results.push_back(sr);
    }
    const double untouchedScore = results[2].score;

    bs::RerankerConfig config;
    config.weight = 4.0f;
    config.maxCandidates = 2;
    config.minScoreThreshold = 0.0f;

    const int boosted = reranker.rerank(QStringLiteral("semantic rerank fixture query"),
                                        results,
                                        config);
    QCOMPARE(boosted, 2);
    for (int i = 0; i < 2; ++i) {
        QVERIFY(results[static_cast<size_t>(i)].crossEncoderScore > 0.0f);
        QVERIFY(results[static_cast<size_t>(i)].crossEncoderScore <= 1.0f);
        QVERIFY(results[static_cast<size_t>(i)].scoreBreakdown.crossEncoderBoost > 0.0);
    }
    QCOMPARE(results[2].scoreBreakdown.crossEncoderBoost, 0.0);
    QCOMPARE(results[2].score, untouchedScore);
}

QTEST_MAIN(TestCrossEncoderReranker)
#include "test_cross_encoder_reranker.moc"
