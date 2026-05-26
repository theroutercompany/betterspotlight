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

#include <cmath>
#include <limits>

namespace {

bool prepareCrossEncoderFixtureModelsDir(const QString& modelsDir)
{
    if (!bs::test::prepareFixtureRerankModelFiles(modelsDir)) {
        return false;
    }

    const QByteArray manifest = R"({
        "models": {
            "cross-encoder": {
                "name": "cross-fixture",
                "modelId": "cross-fixture-v1",
                "generationId": "v1",
                "file": "mxbai-rerank-xsmall-v1-int8.onnx",
                "vocab": "vocab.txt",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask"],
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
    void testCandidateLogitOffsetsValidateOutputShape();
    void testSigmoidScoreRejectsNonFiniteLogits();
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

void TestCrossEncoderReranker::testCandidateLogitOffsetsValidateOutputShape()
{
    using bs::cross_encoder_detail::candidateLogitOffsets;

    auto flat = candidateLogitOffsets({3}, 3, 3);
    QVERIFY(flat.has_value());
    QCOMPARE(flat->at(0), static_cast<size_t>(0));
    QCOMPARE(flat->at(1), static_cast<size_t>(1));
    QCOMPARE(flat->at(2), static_cast<size_t>(2));

    auto singleColumn = candidateLogitOffsets({3, 1}, 3, 3);
    QVERIFY(singleColumn.has_value());
    QCOMPARE(singleColumn->at(0), static_cast<size_t>(0));
    QCOMPARE(singleColumn->at(1), static_cast<size_t>(1));
    QCOMPARE(singleColumn->at(2), static_cast<size_t>(2));

    auto twoClass = candidateLogitOffsets({3, 2}, 3, 6);
    QVERIFY(twoClass.has_value());
    QCOMPARE(twoClass->at(0), static_cast<size_t>(1));
    QCOMPARE(twoClass->at(1), static_cast<size_t>(3));
    QCOMPARE(twoClass->at(2), static_cast<size_t>(5));

    QVERIFY(!candidateLogitOffsets({1}, 3, 1).has_value());
    QVERIFY(!candidateLogitOffsets({3, 2}, 3, 5).has_value());
    QVERIFY(!candidateLogitOffsets({3, 0}, 3, 0).has_value());
    QVERIFY(!candidateLogitOffsets({-1, 2}, 3, 6).has_value());
    QVERIFY(!candidateLogitOffsets({3, 1, 1}, 3, 3).has_value());
}

void TestCrossEncoderReranker::testSigmoidScoreRejectsNonFiniteLogits()
{
    using bs::cross_encoder_detail::sigmoidScoreFromLogit;

    const auto zero = sigmoidScoreFromLogit(0.0F);
    QVERIFY(zero.has_value());
    QVERIFY(std::abs(*zero - 0.5F) < 0.0001F);

    const auto high = sigmoidScoreFromLogit(120.0F);
    QVERIFY(high.has_value());
    QVERIFY(*high <= 1.0F);
    QVERIFY(*high > 0.999F);

    const auto low = sigmoidScoreFromLogit(-120.0F);
    QVERIFY(low.has_value());
    QVERIFY(*low >= 0.0F);
    QVERIFY(*low < 0.001F);

    QVERIFY(!sigmoidScoreFromLogit(std::numeric_limits<float>::quiet_NaN()).has_value());
    QVERIFY(!sigmoidScoreFromLogit(std::numeric_limits<float>::infinity()).has_value());
    QVERIFY(!sigmoidScoreFromLogit(-std::numeric_limits<float>::infinity()).has_value());
}

QTEST_MAIN(TestCrossEncoderReranker)
#include "test_cross_encoder_reranker.moc"
