#include <QtTest/QtTest>

#include "core/models/model_manifest.h"
#include "core/models/model_session.h"

#include <QFile>
#include <QTemporaryDir>

class TestModelSession : public QObject {
    Q_OBJECT

private slots:
    void testInitializeFailsWhenModelPathMissing();
    void testMetadataAccessorsRemainStableOnFailure();
    void testInitializeReadsCoreMlDisableEnv();
};

void TestModelSession::testInitializeFailsWhenModelPathMissing()
{
    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("unit-test-model");
    entry.file = QStringLiteral("missing.onnx");
    entry.inputs = {QStringLiteral("input_ids"), QStringLiteral("attention_mask")};

    bs::ModelSession session(entry);
    QVERIFY(!session.initialize(QStringLiteral("/no/such/model.onnx")));
    QVERIFY(!session.isAvailable());
    QVERIFY(session.rawSession() == nullptr);
}

void TestModelSession::testMetadataAccessorsRemainStableOnFailure()
{
    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("qa-extractive");
    entry.file = QStringLiteral("qa.onnx");
    entry.providerPolicy.preferredProvider = QStringLiteral("cpu");
    entry.providerPolicy.preferCoreMl = false;

    bs::ModelSession session(entry);
    QVERIFY(!session.initialize(QString()));

    QCOMPARE(session.manifest().name, QStringLiteral("qa-extractive"));
    QVERIFY(session.outputNames().empty());
    QCOMPARE(QString::fromStdString(session.selectedProvider()), QStringLiteral("cpu"));
    QVERIFY(!session.coreMlAttached());
    QVERIFY(session.rawSession() == nullptr);
}

void TestModelSession::testInitializeReadsCoreMlDisableEnv()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString modelPath = tempDir.filePath(QStringLiteral("fake.onnx"));
    QFile modelFile(modelPath);
    QVERIFY(modelFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    modelFile.write("not-a-real-onnx-model");
    modelFile.close();

    const char* envKey = "BS_TEST_DISABLE_COREML_FOR_MODELSESSION";
    const bool hadOriginal = qEnvironmentVariableIsSet(envKey);
    const QByteArray originalValue = qgetenv(envKey);

    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("env-coreml");
    entry.file = QStringLiteral("fake.onnx");
    entry.inputs = {QStringLiteral("input_ids")};
    entry.providerPolicy.preferredProvider = QStringLiteral("coreml");
    entry.providerPolicy.preferCoreMl = true;
    entry.providerPolicy.disableCoreMlEnvVar = QString::fromLatin1(envKey);

    qputenv(envKey, QByteArrayLiteral("yes"));
    {
        bs::ModelSession disabledSession(entry);
        QVERIFY(!disabledSession.initialize(modelPath));
        QVERIFY(!disabledSession.isAvailable());
        QVERIFY(!disabledSession.coreMlRequested());
    }

    qunsetenv(envKey);
    {
        bs::ModelSession enabledSession(entry);
        QVERIFY(!enabledSession.initialize(modelPath));
        QVERIFY(!enabledSession.isAvailable());
        QVERIFY(enabledSession.coreMlRequested());
    }

    if (hadOriginal) {
        qputenv(envKey, originalValue);
    } else {
        qunsetenv(envKey);
    }
}

QTEST_MAIN(TestModelSession)
#include "test_model_session.moc"
