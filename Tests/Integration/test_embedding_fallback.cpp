#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"
#include "core/vector/search_merger.h"

class TestEmbeddingFallback : public QObject {
    Q_OBJECT

private slots:
    void testFallbackToLexicalOnly();
};

void TestEmbeddingFallback::testFallbackToLexicalOnly()
{
    bs::EmbeddingManager manager(QStringLiteral("missing_model.onnx"),
                                 QStringLiteral("missing_vocab.txt"));
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

QTEST_MAIN(TestEmbeddingFallback)
#include "test_embedding_fallback.moc"
