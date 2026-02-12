#include <QtTest/QtTest>
#include "core/vector/vector_index.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <limits>
#include <vector>

class TestVectorIndex : public QObject {
    Q_OBJECT

private slots:
    void testCreateIndex();
    void testGuardClausesAndInvalidMetadata();
    void testAddAndSearch();
    void testAddMultipleVectors();
    void testDeleteVector();
    void testSearchEmptyIndex();
    void testSearchKParameter();
    void testTotalElements();
    void testNeedsRebuild();
    void testResizeWhenCapacityThresholdReached();
    void testSaveAndLoad();
    void testLoadRejectsDimensionMismatch();
    void testLoadRejectsInvalidMetaFiles();
    void testLoadRejectsCorruptedIndexPayload();
    void testLoadSupportsLegacyModelMetadata();

private:
    static constexpr int kTestDimensions = 384;
    static std::vector<float> makeVector(int seed);
};

std::vector<float> TestVectorIndex::makeVector(int seed)
{
    std::vector<float> vector(static_cast<size_t>(kTestDimensions), 0.0F);
    vector[static_cast<size_t>(seed % kTestDimensions)] = 1.0F;
    return vector;
}

void TestVectorIndex::testCreateIndex()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());
    QVERIFY(index.isAvailable());
}

void TestVectorIndex::testGuardClausesAndInvalidMetadata()
{
    bs::VectorIndex unconfigured;
    QVERIFY(!unconfigured.create());
    QVERIFY(!unconfigured.save("/tmp/vector-index-unavailable.bin",
                               "/tmp/vector-index-unavailable.meta.json"));
    QCOMPARE(unconfigured.totalElements(), 0);
    QCOMPARE(unconfigured.deletedElements(), 0);
    QVERIFY(!unconfigured.needsRebuild());
    QVERIFY(!unconfigured.isAvailable());
    QCOMPARE(unconfigured.nextLabel(), static_cast<uint64_t>(0));

    bs::VectorIndex::IndexMetadata invalidMeta;
    invalidMeta.dimensions = 0;
    invalidMeta.modelId = "invalid";
    invalidMeta.generationId = "v0";
    bs::VectorIndex invalid(invalidMeta);
    QVERIFY(!invalid.configure(invalidMeta));
    QVERIFY(!invalid.create());

    bs::VectorIndex::IndexMetadata validMeta;
    validMeta.dimensions = kTestDimensions;
    validMeta.modelId = "unit-test-model";
    validMeta.generationId = "v1";
    bs::VectorIndex index(validMeta);
    QVERIFY(index.create());
    QVERIFY(!index.configure(validMeta));

    const uint64_t nullAdd = index.addVector(nullptr);
    QCOMPARE(nullAdd, std::numeric_limits<uint64_t>::max());
    QVERIFY(!unconfigured.deleteVector(1));
    QVERIFY(index.search(nullptr, 3).empty());
    QVERIFY(index.search(makeVector(1).data(), 0).empty());
}

void TestVectorIndex::testAddAndSearch()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    for (int i = 0; i < 5; ++i) {
        const std::vector<float> vec = makeVector(i);
        const uint64_t label = index.addVector(vec.data());
        QVERIFY(label != std::numeric_limits<uint64_t>::max());
    }

    const std::vector<float> query = makeVector(2);
    const std::vector<bs::VectorIndex::KnnResult> hits = index.search(query.data(), 3);
    QVERIFY(!hits.empty());
    QVERIFY(hits[0].label <= 4U);
}

void TestVectorIndex::testAddMultipleVectors()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    for (int i = 0; i < 100; ++i) {
        const std::vector<float> vec = makeVector(i);
        index.addVector(vec.data());
    }
    QCOMPARE(index.totalElements(), 100);
}

void TestVectorIndex::testSearchKParameter()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    for (int i = 0; i < 20; ++i) {
        const std::vector<float> vec = makeVector(i);
        index.addVector(vec.data());
    }

    const std::vector<float> query = makeVector(0);
    const std::vector<bs::VectorIndex::KnnResult> hits = index.search(query.data(), 5);
    QVERIFY(static_cast<int>(hits.size()) <= 5);
}

void TestVectorIndex::testDeleteVector()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    std::vector<uint64_t> labels;
    for (int i = 0; i < 3; ++i) {
        const std::vector<float> vec = makeVector(i);
        labels.push_back(index.addVector(vec.data()));
    }

    QVERIFY(index.deleteVector(labels[1]));
    QCOMPARE(index.deletedElements(), 1);
}

void TestVectorIndex::testSearchEmptyIndex()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    const std::vector<float> query = makeVector(0);
    const std::vector<bs::VectorIndex::KnnResult> hits = index.search(query.data(), 5);
    QVERIFY(hits.empty());
}

void TestVectorIndex::testTotalElements()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    for (int i = 0; i < 10; ++i) {
        const std::vector<float> vec = makeVector(i);
        index.addVector(vec.data());
    }

    QCOMPARE(index.totalElements(), 10);
}

void TestVectorIndex::testNeedsRebuild()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    std::vector<uint64_t> labels;
    labels.reserve(100);
    for (int i = 0; i < 100; ++i) {
        const std::vector<float> vec = makeVector(i);
        labels.push_back(index.addVector(vec.data()));
    }

    for (int i = 0; i < 50; ++i) {
        QVERIFY(index.deleteVector(labels[static_cast<size_t>(i)]));
    }

    QVERIFY(index.needsRebuild());
}

void TestVectorIndex::testResizeWhenCapacityThresholdReached()
{
    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create(1));

    const uint64_t first = index.addVector(makeVector(0).data());
    const uint64_t second = index.addVector(makeVector(1).data());
    QVERIFY(first != std::numeric_limits<uint64_t>::max());
    QVERIFY(second != std::numeric_limits<uint64_t>::max());
    QCOMPARE(index.totalElements(), 2);
}

void TestVectorIndex::testSaveAndLoad()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    for (int i = 0; i < 8; ++i) {
        const std::vector<float> vec = makeVector(i);
        index.addVector(vec.data());
    }

    const QString indexPath = tempDir.path() + QStringLiteral("/index.bin");
    const QString metaPath = tempDir.path() + QStringLiteral("/index.meta.json");
    QVERIFY(index.save(indexPath.toStdString(), metaPath.toStdString()));

    bs::VectorIndex loaded(meta);
    QVERIFY(loaded.load(indexPath.toStdString(), metaPath.toStdString()));
    QCOMPARE(loaded.totalElements(), index.totalElements());
    QCOMPARE(loaded.metadata().modelId, meta.modelId);
    QCOMPARE(loaded.metadata().generationId, meta.generationId);
    QCOMPARE(loaded.metadata().provider, meta.provider);
    QCOMPARE(loaded.nextLabel(), index.nextLabel());
}

void TestVectorIndex::testLoadRejectsDimensionMismatch()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    bs::VectorIndex::IndexMetadata sourceMeta;
    sourceMeta.dimensions = kTestDimensions;
    sourceMeta.modelId = "source-model";
    sourceMeta.generationId = "v1";

    bs::VectorIndex source(sourceMeta);
    QVERIFY(source.create());
    for (int i = 0; i < 4; ++i) {
        source.addVector(makeVector(i).data());
    }

    const QString indexPath = tempDir.path() + QStringLiteral("/mismatch.bin");
    const QString metaPath = tempDir.path() + QStringLiteral("/mismatch.meta.json");
    QVERIFY(source.save(indexPath.toStdString(), metaPath.toStdString()));

    bs::VectorIndex::IndexMetadata targetMeta;
    targetMeta.dimensions = 1024;
    targetMeta.modelId = "target-model";
    targetMeta.generationId = "v2";
    bs::VectorIndex target(targetMeta);
    QVERIFY(!target.load(indexPath.toStdString(), metaPath.toStdString()));
}

void TestVectorIndex::testLoadRejectsInvalidMetaFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString indexPath = tempDir.path() + QStringLiteral("/missing.bin");
    const QString badMetaPath = tempDir.path() + QStringLiteral("/bad.meta.json");
    {
        QFile metaFile(badMetaPath);
        QVERIFY(metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metaFile.write("{not-json");
        metaFile.close();
    }

    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    bs::VectorIndex index(meta);
    QVERIFY(!index.load(indexPath.toStdString(), badMetaPath.toStdString()));

    const QString missingDimMetaPath = tempDir.path() + QStringLiteral("/missing-dim.meta.json");
    {
        QFile metaFile(missingDimMetaPath);
        QVERIFY(metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metaFile.write("{\"version\":2,\"model_id\":\"m\",\"generation_id\":\"v1\"}");
        metaFile.close();
    }
    QVERIFY(!index.load(indexPath.toStdString(), missingDimMetaPath.toStdString()));
}

void TestVectorIndex::testLoadRejectsCorruptedIndexPayload()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString indexPath = tempDir.path() + QStringLiteral("/corrupt.bin");
    const QString metaPath = tempDir.path() + QStringLiteral("/corrupt.meta.json");

    {
        QFile indexFile(indexPath);
        QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        indexFile.write("not-a-valid-hnsw-index");
        indexFile.close();
    }
    {
        const QJsonObject meta = {
            {QStringLiteral("version"), 2},
            {QStringLiteral("dimensions"), kTestDimensions},
            {QStringLiteral("model_id"), QStringLiteral("unit-test-model")},
            {QStringLiteral("generation_id"), QStringLiteral("v1")},
            {QStringLiteral("provider"), QStringLiteral("cpu")},
            {QStringLiteral("total_elements"), 1},
            {QStringLiteral("next_label"), 1},
            {QStringLiteral("deleted_elements"), 0},
            {QStringLiteral("ef_construction"), bs::VectorIndex::kEfConstruction},
            {QStringLiteral("m"), bs::VectorIndex::kM},
        };

        QFile metaFile(metaPath);
        QVERIFY(metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
        metaFile.close();
    }

    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kTestDimensions;
    meta.modelId = "unit-test-model";
    meta.generationId = "v1";
    meta.provider = "cpu";
    bs::VectorIndex index(meta);
    QVERIFY(!index.load(indexPath.toStdString(), metaPath.toStdString()));
}

void TestVectorIndex::testLoadSupportsLegacyModelMetadata()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    bs::VectorIndex::IndexMetadata sourceMeta;
    sourceMeta.dimensions = kTestDimensions;
    sourceMeta.modelId = "modern-model";
    sourceMeta.generationId = "g2";
    sourceMeta.provider = "neural";

    bs::VectorIndex source(sourceMeta);
    QVERIFY(source.create());
    source.addVector(makeVector(0).data());
    source.addVector(makeVector(1).data());

    const QString indexPath = tempDir.path() + QStringLiteral("/legacy.bin");
    const QString metaPath = tempDir.path() + QStringLiteral("/legacy.meta.json");
    QVERIFY(source.save(indexPath.toStdString(), metaPath.toStdString()));

    {
        QFile metaFile(metaPath);
        QVERIFY(metaFile.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll(), &parseError);
        metaFile.close();
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(doc.isObject());

        QJsonObject obj = doc.object();
        obj.insert(QStringLiteral("version"), 7);
        obj.insert(QStringLiteral("model"), QStringLiteral("legacy-model"));
        obj.remove(QStringLiteral("model_id"));
        obj.remove(QStringLiteral("generation_id"));
        obj.remove(QStringLiteral("provider"));
        obj.insert(QStringLiteral("next_label"), 42);

        QVERIFY(metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metaFile.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        metaFile.close();
    }

    bs::VectorIndex::IndexMetadata loadMeta;
    loadMeta.dimensions = kTestDimensions;
    bs::VectorIndex loaded(loadMeta);
    QVERIFY(loaded.load(indexPath.toStdString(), metaPath.toStdString()));
    QCOMPARE(loaded.metadata().schemaVersion, 7);
    QCOMPARE(loaded.metadata().modelId, std::string("legacy-model"));
    QCOMPARE(loaded.metadata().generationId, std::string("v1"));
    QCOMPARE(loaded.metadata().provider, std::string("cpu"));
    QCOMPARE(loaded.nextLabel(), static_cast<uint64_t>(42));

    const uint64_t label = loaded.addVector(makeVector(2).data());
    QCOMPARE(label, static_cast<uint64_t>(42));
}

QTEST_MAIN(TestVectorIndex)
#include "test_vector_index.moc"
