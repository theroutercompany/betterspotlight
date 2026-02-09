#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"
#include "core/vector/search_merger.h"

class TestEmbeddingFallback : public QObject {
    Q_OBJECT

private slots:
    void testNoModelGracefulFallback();
    void testEmbedFailureReturnsFTS5();
};

void TestEmbeddingFallback::testNoModelGracefulFallback()
{
    bs::ModelRegistry registry(QStringLiteral("/nonexistent/models"));
    bs::EmbeddingManager manager(&registry);
    QVERIFY(!manager.initialize());
    QVERIFY(!manager.isAvailable());

    std::vector<bs::SearchResult> lexical;
    bs::SearchResult result;
    result.itemId = 1;
    result.path = QStringLiteral("/tmp/lexical.txt");
    result.name = QStringLiteral("lexical.txt");
    result.score = 50.0;
    result.matchType = bs::MatchType::Content;
    lexical.push_back(result);

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, {});
    QCOMPARE(static_cast<int>(merged.size()), 1);
    QCOMPARE(merged[0].itemId, static_cast<int64_t>(1));
}

void TestEmbeddingFallback::testEmbedFailureReturnsFTS5()
{
    bs::EmbeddingManager manager(nullptr);

    const std::vector<float> embedding = manager.embed(QStringLiteral("test"));
    QVERIFY(embedding.empty());

    std::vector<bs::SearchResult> lexical;
    bs::SearchResult r1;
    r1.itemId = 10;
    r1.path = QStringLiteral("/src/main.cpp");
    r1.name = QStringLiteral("main.cpp");
    r1.score = 200.0;
    r1.matchType = bs::MatchType::ExactName;
    lexical.push_back(r1);

    bs::SearchResult r2;
    r2.itemId = 20;
    r2.path = QStringLiteral("/src/utils.h");
    r2.name = QStringLiteral("utils.h");
    r2.score = 100.0;
    r2.matchType = bs::MatchType::ContainsName;
    lexical.push_back(r2);

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, {});
    QCOMPARE(static_cast<int>(merged.size()), 2);
    QCOMPARE(merged[0].itemId, static_cast<int64_t>(10));
    QCOMPARE(merged[1].itemId, static_cast<int64_t>(20));
}

QTEST_MAIN(TestEmbeddingFallback)
#include "test_embedding_fallback.moc"
