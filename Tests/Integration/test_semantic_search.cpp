#include <QtTest/QtTest>
#include "core/vector/vector_index.h"
#include "core/vector/search_merger.h"

#include <algorithm>
#include <vector>

class TestSemanticSearch : public QObject {
    Q_OBJECT

private slots:
    void testSemanticMergeEndToEnd();

private:
    static std::vector<float> makeVector(int seed);
};

std::vector<float> TestSemanticSearch::makeVector(int seed)
{
    std::vector<float> vector(static_cast<size_t>(bs::VectorIndex::kDimensions), 0.0F);
    vector[static_cast<size_t>(seed % bs::VectorIndex::kDimensions)] = 1.0F;
    return vector;
}

void TestSemanticSearch::testSemanticMergeEndToEnd()
{
    bs::VectorIndex index;
    QVERIFY(index.create());

    const uint64_t l0 = index.addVector(makeVector(0).data());
    const uint64_t l1 = index.addVector(makeVector(1).data());
    const uint64_t l2 = index.addVector(makeVector(2).data());
    Q_UNUSED(l0);
    Q_UNUSED(l1);
    Q_UNUSED(l2);

    const std::vector<bs::VectorIndex::KnnResult> knn = index.search(makeVector(1).data(), 2);
    QVERIFY(!knn.empty());

    std::vector<bs::SemanticResult> semantic;
    semantic.reserve(knn.size());
    for (const auto& hit : knn) {
        semantic.push_back({static_cast<int64_t>(hit.label + 1000), 0.9F});
    }

    std::vector<bs::SearchResult> lexical;
    bs::SearchResult lexicalResult;
    lexicalResult.itemId = 500;
    lexicalResult.path = QStringLiteral("/docs/readme.md");
    lexicalResult.name = QStringLiteral("readme.md");
    lexicalResult.score = 120.0;
    lexicalResult.matchType = bs::MatchType::Content;
    lexical.push_back(lexicalResult);

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QVERIFY(!merged.empty());

    const bool hasLexical = std::any_of(merged.begin(), merged.end(),
        [](const bs::SearchResult& result) { return result.itemId == 500; });
    const bool hasSemantic = std::any_of(merged.begin(), merged.end(),
        [](const bs::SearchResult& result) { return result.itemId >= 1000; });
    QVERIFY(hasLexical);
    QVERIFY(hasSemantic);
}

QTEST_MAIN(TestSemanticSearch)
#include "test_semantic_search.moc"
