#include <QtTest/QtTest>

#include "core/models/model_registry.h"
#include "core/ranking/qa_extractive_model.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>

namespace {

QString fixtureModelsSourceDir()
{
    const QString resolved = bs::ModelRegistry::resolveModelsDir();
    const QString resolvedModel =
        QDir(resolved).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    const QString resolvedVocab = QDir(resolved).filePath(QStringLiteral("vocab.txt"));
    if (QFileInfo::exists(resolvedModel) && QFileInfo::exists(resolvedVocab)) {
        return resolved;
    }

    return QStringLiteral("/Users/rexliu/betterspotlight/data/models");
}

bool linkOrCopyFile(const QString& sourcePath, const QString& targetPath)
{
    QFile::remove(targetPath);
    if (QFile::link(sourcePath, targetPath)) {
        return true;
    }
    return QFile::copy(sourcePath, targetPath);
}

bool prepareQaFixtureModelsDir(const QString& modelsDir)
{
    const QString sourceDir = fixtureModelsSourceDir();
    const QString modelSource =
        QDir(sourceDir).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx"));
    const QString vocabSource = QDir(sourceDir).filePath(QStringLiteral("vocab.txt"));
    if (!QFileInfo::exists(modelSource) || !QFileInfo::exists(vocabSource)) {
        return false;
    }

    if (!linkOrCopyFile(modelSource,
                        QDir(modelsDir).filePath(QStringLiteral("bge-small-en-v1.5-int8.onnx")))) {
        return false;
    }
    if (!linkOrCopyFile(vocabSource, QDir(modelsDir).filePath(QStringLiteral("vocab.txt")))) {
        return false;
    }

    const QByteArray manifest = R"({
        "models": {
            "qa-extractive": {
                "name": "qa-fixture",
                "modelId": "qa-fixture-v1",
                "generationId": "v1",
                "file": "bge-small-en-v1.5-int8.onnx",
                "vocab": "vocab.txt",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask", "token_type_ids"],
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
    const QByteArray oldQaFallback = qgetenv("BS_TEST_QA_SINGLE_OUTPUT_FALLBACK");
    qputenv("BETTERSPOTLIGHT_DISABLE_COREML", QByteArrayLiteral("1"));
    qputenv("BS_TEST_QA_SINGLE_OUTPUT_FALLBACK", QByteArrayLiteral("1"));
    const auto restoreEnv = qScopeGuard([&]() {
        if (oldDisableCoreMl.isNull()) {
            qunsetenv("BETTERSPOTLIGHT_DISABLE_COREML");
        } else {
            qputenv("BETTERSPOTLIGHT_DISABLE_COREML", oldDisableCoreMl);
        }
        if (oldQaFallback.isNull()) {
            qunsetenv("BS_TEST_QA_SINGLE_OUTPUT_FALLBACK");
        } else {
            qputenv("BS_TEST_QA_SINGLE_OUTPUT_FALLBACK", oldQaFallback);
        }
    });
    Q_UNUSED(restoreEnv);

    bs::ModelRegistry registry(modelsDir.path());
    bs::QaExtractiveModel fixtureModel(&registry, "qa-extractive");
    QVERIFY2(fixtureModel.initialize(), "QA fixture should initialize under single-output fallback");
    QVERIFY(fixtureModel.isAvailable());

    const auto emptyQuery = fixtureModel.extract(
        QString(), QStringLiteral("non-empty context for qa extraction"));
    QVERIFY(!emptyQuery.available);
    QCOMPARE(emptyQuery.startToken, -1);
    QCOMPARE(emptyQuery.endToken, -1);

    const auto fixtureAnswer = fixtureModel.extract(
        QStringLiteral("What happened in the quarterly report?"),
        QStringLiteral("First sentence about setup. Second sentence contains the quarterly report "
                       "summary and key remediation details. Third sentence closes the context."),
        180);
    QVERIFY(fixtureAnswer.available);
    QVERIFY(fixtureAnswer.startToken >= 0);
    QVERIFY(fixtureAnswer.endToken >= fixtureAnswer.startToken);
    QVERIFY(!fixtureAnswer.answer.trimmed().isEmpty());
    QVERIFY(fixtureAnswer.confidence >= 0.0);
    QVERIFY(fixtureAnswer.confidence <= 1.0);
}

QTEST_MAIN(TestQaExtractiveModel)
#include "test_qa_extractive_model.moc"
