#include <QtTest/QtTest>

#include "core/models/model_registry.h"
#include "core/ranking/qa_extractive_model.h"
#include "../Utils/model_fixture_paths.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <limits>

namespace {

bool prepareQaFixtureModelsDir(const QString& modelsDir)
{
    const QString sourceDir = bs::test::fixtureModelsSourceDir();
    if (sourceDir.isEmpty()) {
        return false;
    }

    QDir().mkpath(modelsDir);
    if (!bs::test::linkOrCopyFile(
            QDir(sourceDir).filePath(QStringLiteral("distilbert-base-cased-distilled-squad-quantized.onnx")),
            QDir(modelsDir).filePath(QStringLiteral("distilbert-base-cased-distilled-squad-quantized.onnx")))) {
        return false;
    }
    if (!bs::test::linkOrCopyFile(
            QDir(sourceDir).filePath(QStringLiteral("vocab.txt")),
            QDir(modelsDir).filePath(QStringLiteral("vocab.txt")))) {
        return false;
    }

    const QByteArray manifest = R"({
        "models": {
            "qa-extractive": {
                "name": "qa-fixture",
                "modelId": "qa-fixture-v1",
                "generationId": "v1",
                "file": "distilbert-base-cased-distilled-squad-quantized.onnx",
                "vocab": "vocab.txt",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask"],
                "outputs": ["start_logits", "end_logits"],
                "task": "qa"
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

class TestQaExtractiveModel : public QObject {
    Q_OBJECT

private slots:
    void testInitializeFailsWithoutRegistry();
    void testExtractUnavailableReturnsEmptyAnswer();
    void testSpanSelectionRejectsNonFiniteLogits();
    void testOutputNameSelectionPrefersSemanticNames();
};

void TestQaExtractiveModel::testInitializeFailsWithoutRegistry()
{
    bs::QaExtractiveModel model(nullptr, "qa-extractive");
    QVERIFY(!model.initialize());
    QVERIFY(!model.isAvailable());
}

void TestQaExtractiveModel::testExtractUnavailableReturnsEmptyAnswer()
{
    bs::QaExtractiveModel unavailableModel(nullptr, "qa-extractive");
    const auto unavailableAnswer =
        unavailableModel.extract(QStringLiteral("what"), QStringLiteral("context"));
    QVERIFY(!unavailableModel.warmup());
    QVERIFY(!unavailableAnswer.available);
    QVERIFY(unavailableAnswer.answer.isEmpty());
    QCOMPARE(unavailableAnswer.startToken, -1);
    QCOMPARE(unavailableAnswer.endToken, -1);

    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());
    QVERIFY2(prepareQaFixtureModelsDir(modelsDir.path()),
             "Failed to prepare fixture models directory for qa-extractive");

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
    bs::QaExtractiveModel fixtureModel(&registry, "qa-extractive");
    QVERIFY2(fixtureModel.initialize(), "QA fixture should initialize with two-input manifest");
    QVERIFY(fixtureModel.isAvailable());
    QVERIFY2(fixtureModel.warmup(), "QA fixture should run a tensor warmup probe");

    const auto emptyQuery = fixtureModel.extract(
        QString(), QStringLiteral("non-empty context for qa extraction"));
    QVERIFY(!emptyQuery.available);
    QCOMPARE(emptyQuery.startToken, -1);
    QCOMPARE(emptyQuery.endToken, -1);

    const auto fixtureAnswer = fixtureModel.extract(
        QStringLiteral("what is the plan"),
        QStringLiteral("the plan is simple. the plan is ready. the plan is clear."),
        180);
    QVERIFY(fixtureAnswer.confidence >= 0.0);
    QVERIFY(fixtureAnswer.confidence <= 1.0);
    if (fixtureAnswer.available) {
        QVERIFY(fixtureAnswer.startToken >= 0);
        QVERIFY(fixtureAnswer.endToken >= fixtureAnswer.startToken);
        QVERIFY(!fixtureAnswer.answer.trimmed().isEmpty());
    } else {
        QCOMPARE(fixtureAnswer.startToken, -1);
        QCOMPARE(fixtureAnswer.endToken, -1);
    }
}

void TestQaExtractiveModel::testSpanSelectionRejectsNonFiniteLogits()
{
    using bs::qa_extractive_detail::confidenceForRawScore;
    using bs::qa_extractive_detail::selectBestSpan;

    const auto invalidConfidence =
        confidenceForRawScore(std::numeric_limits<double>::quiet_NaN());
    QCOMPARE(invalidConfidence, 0.0);

    const std::array<float, 5> startLogits = {
        0.1F,
        std::numeric_limits<float>::quiet_NaN(),
        4.0F,
        std::numeric_limits<float>::infinity(),
        0.2F,
    };
    const std::array<float, 5> endLogits = {
        0.1F,
        0.2F,
        0.3F,
        std::numeric_limits<float>::quiet_NaN(),
        5.0F,
    };

    const auto best =
        selectBestSpan(startLogits.data(), endLogits.data(), 0, 4, 3);
    QVERIFY(best.available);
    QCOMPARE(best.startToken, 2);
    QCOMPARE(best.endToken, 4);
    QCOMPARE(best.rawScore, 9.0);
    QVERIFY(std::isfinite(best.confidence));
    QVERIFY(best.confidence >= 0.0);
    QVERIFY(best.confidence <= 1.0);

    const auto shorterSpan =
        selectBestSpan(startLogits.data(), endLogits.data(), 0, 4, 2);
    QVERIFY(shorterSpan.available);
    QCOMPARE(shorterSpan.startToken, 4);
    QCOMPARE(shorterSpan.endToken, 4);
    QVERIFY(std::abs(shorterSpan.rawScore - 5.2) < 0.0001);

    const std::array<float, 2> noFiniteStart = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
    };
    const std::array<float, 2> finiteEnd = {1.0F, 2.0F};
    const auto unavailable =
        selectBestSpan(noFiniteStart.data(), finiteEnd.data(), 0, 1, 1);
    QVERIFY(!unavailable.available);
    QCOMPARE(unavailable.startToken, -1);
    QCOMPARE(unavailable.endToken, -1);
    QCOMPARE(unavailable.rawScore, 0.0);
    QCOMPARE(unavailable.confidence, 0.0);

    QVERIFY(!selectBestSpan(nullptr, finiteEnd.data(), 0, 1, 1).available);
    QVERIFY(!selectBestSpan(finiteEnd.data(), nullptr, 0, 1, 1).available);
    QVERIFY(!selectBestSpan(finiteEnd.data(), finiteEnd.data(), 2, 1, 1).available);
    QVERIFY(!selectBestSpan(finiteEnd.data(), finiteEnd.data(), 0, 1, 0).available);
}

void TestQaExtractiveModel::testOutputNameSelectionPrefersSemanticNames()
{
    using bs::qa_extractive_detail::selectOutputNames;

    const auto reversed = selectOutputNames(
        {std::string("end_logits"), std::string("start_logits")},
        false);
    QVERIFY(reversed.available);
    QCOMPARE(QString::fromStdString(reversed.startOutputName),
             QStringLiteral("start_logits"));
    QCOMPARE(QString::fromStdString(reversed.endOutputName),
             QStringLiteral("end_logits"));

    const auto positional = selectOutputNames(
        {std::string("logits_0"), std::string("logits_1")},
        false);
    QVERIFY(positional.available);
    QCOMPARE(QString::fromStdString(positional.startOutputName),
             QStringLiteral("logits_0"));
    QCOMPARE(QString::fromStdString(positional.endOutputName),
             QStringLiteral("logits_1"));

    const auto singleWithoutFallback =
        selectOutputNames({std::string("logits")}, false);
    QVERIFY(!singleWithoutFallback.available);

    const auto singleWithFallback =
        selectOutputNames({std::string("logits")}, true);
    QVERIFY(singleWithFallback.available);
    QCOMPARE(QString::fromStdString(singleWithFallback.startOutputName),
             QStringLiteral("logits"));
    QCOMPARE(QString::fromStdString(singleWithFallback.endOutputName),
             QStringLiteral("logits"));
}

QTEST_MAIN(TestQaExtractiveModel)
#include "test_qa_extractive_model.moc"
