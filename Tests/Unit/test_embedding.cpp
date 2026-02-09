#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"

class TestEmbedding : public QObject {
    Q_OBJECT

private slots:
    void testConstructWithMissingModel();
    void testEmbedWithoutInit();
    void testQueryPrefixAdded();
    void testEmbedBatchWithoutModel();
    void testInitializeWithBadModel();
};

void TestEmbedding::testConstructWithMissingModel()
{
    bs::ModelRegistry registry(QStringLiteral("/nonexistent/models"));
    bs::EmbeddingManager manager(&registry);
    QVERIFY(!manager.initialize());
    QVERIFY(!manager.isAvailable());
}

void TestEmbedding::testEmbedWithoutInit()
{
    bs::EmbeddingManager manager(nullptr);
    const std::vector<float> embedding = manager.embed(QStringLiteral("hello"));
    QVERIFY(embedding.empty());
}

void TestEmbedding::testQueryPrefixAdded()
{
    bs::EmbeddingManager manager(nullptr);

    const std::vector<float> queryEmbedding = manager.embedQuery(QStringLiteral("query text"));
    QVERIFY(queryEmbedding.empty());
    QVERIFY(!manager.isAvailable());
}

void TestEmbedding::testEmbedBatchWithoutModel()
{
    bs::EmbeddingManager manager(nullptr);

    std::vector<QString> texts = {
        QStringLiteral("hello"),
        QStringLiteral("world"),
        QStringLiteral("test"),
    };
    const std::vector<std::vector<float>> results = manager.embedBatch(texts);
    QVERIFY(results.empty());
}

void TestEmbedding::testInitializeWithBadModel()
{
    bs::ModelRegistry registry(QStringLiteral("/nonexistent/path"));
    bs::EmbeddingManager manager(&registry);
    bool ok = manager.initialize();
    QVERIFY(!ok);
    QVERIFY(!manager.isAvailable());

    const std::vector<float> embedding = manager.embed(QStringLiteral("test"));
    QVERIFY(embedding.empty());
}

QTEST_MAIN(TestEmbedding)
#include "test_embedding.moc"
