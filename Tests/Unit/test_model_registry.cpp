#include <QtTest/QtTest>

#include "core/models/model_manifest.h"
#include "core/models/model_registry.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTemporaryFile>

class TestModelRegistry : public QObject {
    Q_OBJECT

private slots:
    void testManifestParsing();
    void testManifestMissingFile();
    void testManifestInvalidJson();
    void testRegistryGetSessionUnknownRole();
};

void TestModelRegistry::testManifestParsing()
{
    const QByteArray json = R"({
        "models": {
            "bi-encoder": {
                "name": "bge-small-en-v1.5",
                "file": "bge-small-en-v1.5-int8.onnx",
                "vocab": "vocab.txt",
                "dimensions": 384,
                "maxSeqLength": 512,
                "queryPrefix": "Represent this sentence: ",
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask", "token_type_ids"],
                "outputs": ["last_hidden_state"],
                "extractionStrategy": "cls_token"
            },
            "cross-encoder": {
                "name": "ms-marco-MiniLM-L-6-v2",
                "file": "ms-marco-minilm.onnx",
                "vocab": "vocab.txt",
                "dimensions": 1,
                "maxSeqLength": 512,
                "tokenizer": "wordpiece",
                "inputs": ["input_ids", "attention_mask"],
                "outputs": ["logits"],
                "extractionStrategy": "single_score"
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

    // Verify cross-encoder entry
    auto ceIt = manifest->models.find("cross-encoder");
    QVERIFY(ceIt != manifest->models.end());
    const bs::ModelManifestEntry& crossEncoder = ceIt->second;
    QCOMPARE(crossEncoder.name, QStringLiteral("ms-marco-MiniLM-L-6-v2"));
    QCOMPARE(crossEncoder.dimensions, 1);
    QCOMPARE(crossEncoder.inputs.size(), 2UL);
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
}

QTEST_MAIN(TestModelRegistry)
#include "test_model_registry.moc"
