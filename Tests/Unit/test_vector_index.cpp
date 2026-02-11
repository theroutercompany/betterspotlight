#include <QtTest/QtTest>
#include "core/vector/vector_index.h"

#include <QTemporaryDir>

#include <limits>
#include <vector>

class TestVectorIndex : public QObject {
    Q_OBJECT

private slots:
    void testCreateIndex();
    void testAddAndSearch();
    void testAddMultipleVectors();
    void testDeleteVector();
    void testSearchEmptyIndex();
    void testSearchKParameter();
    void testTotalElements();
    void testNeedsRebuild();
    void testSaveAndLoad();
    void testLoadRejectsDimensionMismatch();

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

QTEST_MAIN(TestVectorIndex)
#include "test_vector_index.moc"
