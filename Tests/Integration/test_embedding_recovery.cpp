#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"
#include "core/vector/vector_index.h"
#include "core/vector/search_merger.h"

#include <QTemporaryDir>

#include <algorithm>
#include <vector>

class TestEmbeddingRecovery : public QObject {
    Q_OBJECT

private slots:
    void testRecoveryAfterModelFailure();
    void testVectorIndexPersistence();

private:
    static std::vector<float> makeVector(int seed);
};

std::vector<float> TestEmbeddingRecovery::makeVector(int seed)
{
    std::vector<float> vec(static_cast<size_t>(bs::VectorIndex::kDimensions), 0.0F);
    vec[static_cast<size_t>(seed % bs::VectorIndex::kDimensions)] = 1.0F;
    return vec;
}

void TestEmbeddingRecovery::testRecoveryAfterModelFailure()
{
    bs::EmbeddingManager manager(QStringLiteral("missing_model.onnx"),
                                 QStringLiteral("missing_vocab.txt"));
    QVERIFY(!manager.initialize());
    QVERIFY(!manager.isAvailable());

    std::vector<bs::SearchResult> lexical;
    bs::SearchResult lexicalResult;
    lexicalResult.itemId = 1;
    lexicalResult.path = QStringLiteral("/tmp/lexical_only.txt");
    lexicalResult.name = QStringLiteral("lexical_only.txt");
    lexicalResult.score = 90.0;
    lexicalResult.matchType = bs::MatchType::Content;
    lexical.push_back(lexicalResult);

    const std::vector<bs::SearchResult> disabledMerged = bs::SearchMerger::merge(lexical, {});
    QCOMPARE(static_cast<int>(disabledMerged.size()), 1);
    QCOMPARE(disabledMerged[0].itemId, static_cast<int64_t>(1));

    std::vector<bs::SemanticResult> recoveredSemantic = {
        {2, 0.95F},
    };

    const std::vector<bs::SearchResult> recoveredMerged =
        bs::SearchMerger::merge(lexical, recoveredSemantic);

    const bool hasSemanticResult = std::any_of(
        recoveredMerged.begin(), recoveredMerged.end(),
        [](const bs::SearchResult& result) { return result.itemId == 2; });
    QVERIFY(hasSemanticResult);
}

void TestEmbeddingRecovery::testVectorIndexPersistence()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const std::string indexPath = tempDir.filePath("recovery.idx").toStdString();
    const std::string metaPath = tempDir.filePath("recovery.meta").toStdString();

    auto queryVec = makeVector(5);

    {
        bs::VectorIndex index;
        QVERIFY(index.create(1000));

        for (int i = 0; i < 10; ++i) {
            index.addVector(makeVector(i).data());
        }
        QCOMPARE(index.totalElements(), 10);
        QVERIFY(index.save(indexPath, metaPath));
    }

    {
        bs::VectorIndex loaded;
        QVERIFY(loaded.load(indexPath, metaPath));
        QVERIFY(loaded.isAvailable());
        QCOMPARE(loaded.totalElements(), 10);

        const auto results = loaded.search(queryVec.data(), 3);
        QVERIFY(!results.empty());
        QVERIFY(results[0].distance < 0.01F);
    }
}

QTEST_MAIN(TestEmbeddingRecovery)
#include "test_embedding_recovery.moc"
