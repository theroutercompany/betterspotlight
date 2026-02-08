#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"
#include "core/vector/search_merger.h"

#include <algorithm>

class TestEmbeddingRecovery : public QObject {
    Q_OBJECT

private slots:
    void testDisableReenableSearch();
};

void TestEmbeddingRecovery::testDisableReenableSearch()
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

QTEST_MAIN(TestEmbeddingRecovery)
#include "test_embedding_recovery.moc"
