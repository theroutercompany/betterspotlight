#include <QtTest/QtTest>

#include "core/models/model_registry.h"
#include "core/ranking/qa_extractive_model.h"
#include "../Utils/model_fixture_paths.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>

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

QTEST_MAIN(TestQaExtractiveModel)
#include "test_qa_extractive_model.moc"
