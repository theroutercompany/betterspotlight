#include <QtTest/QtTest>

#include "core/models/model_registry.h"
#include "core/ranking/cross_encoder_reranker.h"
#include "core/ranking/reranker_cascade.h"
#include "../Utils/model_fixture_paths.h"

#include <QDir>
#include <QFile>
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

std::vector<bs::SearchResult> buildResultsForCascade(int count)
{
    std::vector<bs::SearchResult> results;
    results.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        bs::SearchResult sr;
        sr.itemId = i + 1;
        sr.path = QStringLiteral("/tmp/doc_%1.txt").arg(i + 1);
        sr.name = QStringLiteral("doc_%1.txt").arg(i + 1);
        sr.snippet = QStringLiteral("reranker cascade fixture snippet %1").arg(i + 1);
        sr.score = 100.0 - static_cast<double>(i) * 0.01;
        results.push_back(sr);
    }
    return results;
}

} // namespace

class TestRerankerCascade : public QObject {
    Q_OBJECT

private slots:
    void testDisabledCascadeNoOps();
    void testAmbiguityDetectionByMargin();

private:
    void runAmbiguityDetectionBySemanticDiversity();
    void runBudgetExhaustedSkipsRerankStages();
    void runStage1AndStage2ExecutionWithFixtureModel();
};

void TestRerankerCascade::testDisabledCascadeNoOps()
{
    std::vector<bs::SearchResult> results(3);
    for (int i = 0; i < 3; ++i) {
        results[static_cast<size_t>(i)].itemId = i + 1;
        results[static_cast<size_t>(i)].score = 100.0 - i;
    }

    bs::RerankerCascadeConfig config;
    config.enabled = false;
    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("test"),
        results,
        nullptr,
        nullptr,
        config,
        0);
    QVERIFY(!stats.stage1Applied);
    QVERIFY(!stats.stage2Applied);
    QVERIFY(!stats.ambiguous);
}

void TestRerankerCascade::testAmbiguityDetectionByMargin()
{
    std::vector<bs::SearchResult> results(3);
    results[0].itemId = 1;
    results[1].itemId = 2;
    results[2].itemId = 3;
    results[0].score = 100.00;
    results[1].score = 99.96;
    results[2].score = 99.10;

    bs::RerankerCascadeConfig config;
    config.enabled = true;
    config.ambiguityMarginThreshold = 0.08f;
    config.rerankBudgetMs = 200;
    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("ambiguous query"),
        results,
        nullptr,
        nullptr,
        config,
        20);

    QVERIFY(stats.ambiguous);
    QVERIFY(!stats.stage1Applied);
    QVERIFY(!stats.stage2Applied);

    // Run additional branch scenarios from an existing slot to avoid stale
    // Qt moc slot metadata in incremental test builds.
    runAmbiguityDetectionBySemanticDiversity();
    runBudgetExhaustedSkipsRerankStages();
    runStage1AndStage2ExecutionWithFixtureModel();
}

void TestRerankerCascade::runAmbiguityDetectionBySemanticDiversity()
{
    std::vector<bs::SearchResult> results(6);
    for (int i = 0; i < 6; ++i) {
        results[static_cast<size_t>(i)].itemId = i + 1;
        results[static_cast<size_t>(i)].score = 200.0 - static_cast<double>(i);
    }

    // Ensure margin path alone is NOT enough.
    results[0].score = 120.0;
    results[1].score = 100.0;

    // Trigger ambiguity via high+low semantic diversity.
    results[0].semanticNormalized = 0.80;
    results[1].semanticNormalized = 0.70;
    results[2].semanticNormalized = 0.60;
    results[3].semanticNormalized = 0.05;
    results[4].semanticNormalized = 0.08;
    results[5].semanticNormalized = 0.10;

    bs::RerankerCascadeConfig config;
    config.enabled = true;
    config.ambiguityMarginThreshold = 0.08f;
    config.rerankBudgetMs = 200;
    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("semantic diversity query"),
        results,
        nullptr,
        nullptr,
        config,
        0);
    QVERIFY(stats.ambiguous);
}

void TestRerankerCascade::runBudgetExhaustedSkipsRerankStages()
{
    std::vector<bs::SearchResult> results = buildResultsForCascade(5);

    bs::RerankerCascadeConfig config;
    config.enabled = true;
    config.rerankBudgetMs = 1;
    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("budget exhausted query"),
        results,
        nullptr,
        nullptr,
        config,
        1);

    QVERIFY(!stats.stage1Applied);
    QVERIFY(!stats.stage2Applied);
    QVERIFY(!stats.ambiguous);
    QVERIFY(stats.elapsedMs >= 0);
}

void TestRerankerCascade::runStage1AndStage2ExecutionWithFixtureModel()
{
    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());
    QVERIFY2(prepareCrossEncoderFixtureModelsDir(modelsDir.path()),
             "Failed to prepare fixture models directory");

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
    bs::CrossEncoderReranker stage1(&registry, "cross-encoder");
    bs::CrossEncoderReranker stage2(&registry, "cross-encoder");
    QVERIFY(stage1.initialize());
    QVERIFY(stage2.initialize());
    QVERIFY(stage1.isAvailable());
    QVERIFY(stage2.isAvailable());

    std::vector<bs::SearchResult> results = buildResultsForCascade(8);
    // Keep semantic diversity so ambiguity still evaluates true after stage1.
    results[0].semanticNormalized = 0.80;
    results[1].semanticNormalized = 0.78;
    results[2].semanticNormalized = 0.60;
    results[3].semanticNormalized = 0.05;
    results[4].semanticNormalized = 0.08;
    results[5].semanticNormalized = 0.10;
    results[6].semanticNormalized = 0.50;
    results[7].semanticNormalized = 0.49;

    bs::RerankerCascadeConfig config;
    config.enabled = true;
    config.rerankBudgetMs = 1000;
    config.stage1MaxCandidates = 5;
    config.stage2MaxCandidates = 3;
    config.stage1Weight = 3.0f;
    config.stage2Weight = 8.0f;
    config.ambiguityMarginThreshold = 0.01f;

    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("cascade fixture query"),
        results,
        &stage1,
        &stage2,
        config,
        0);

    QVERIFY(stats.stage1Applied);
    QVERIFY(stats.stage1Depth > 0);
    QVERIFY(stats.ambiguous);
    QVERIFY(stats.stage2Applied);
    QVERIFY(stats.stage2Depth > 0);
    QVERIFY(stats.elapsedMs >= 0);
}

QTEST_MAIN(TestRerankerCascade)
#include "test_reranker_cascade.moc"
