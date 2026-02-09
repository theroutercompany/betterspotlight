#include <QtTest/QtTest>
#include "core/ranking/cross_encoder_reranker.h"
#include "core/shared/search_result.h"

class TestCrossEncoderReranker : public QObject {
    Q_OBJECT

private slots:
    void testConstructWithoutModel();
    void testRerankWithUnavailableModel();
    void testMaxCandidatesCapping();
};

void TestCrossEncoderReranker::testConstructWithoutModel()
{
    // No registry at all — initialize should fail gracefully
    bs::CrossEncoderReranker reranker(nullptr);
    QVERIFY(!reranker.initialize());
    QVERIFY(!reranker.isAvailable());
}

void TestCrossEncoderReranker::testRerankWithUnavailableModel()
{
    bs::CrossEncoderReranker reranker(nullptr);
    QVERIFY(!reranker.initialize());

    // Create some results
    std::vector<bs::SearchResult> results;
    for (int i = 0; i < 5; ++i) {
        bs::SearchResult sr;
        sr.itemId = i + 1;
        sr.path = QStringLiteral("/home/user/doc_%1.pdf").arg(i);
        sr.name = QStringLiteral("doc_%1.pdf").arg(i);
        sr.score = 100.0 - i * 10.0;
        results.push_back(sr);
    }

    // Capture original scores
    std::vector<double> originalScores;
    for (const auto& r : results) {
        originalScores.push_back(r.score);
    }

    // rerank should return 0 and leave results unchanged
    const int boosted = reranker.rerank(QStringLiteral("test query"), results);
    QCOMPARE(boosted, 0);

    for (size_t i = 0; i < results.size(); ++i) {
        QCOMPARE(results[i].score, originalScores[i]);
        QCOMPARE(results[i].scoreBreakdown.crossEncoderBoost, 0.0);
        QCOMPARE(results[i].crossEncoderScore, 0.0f);
    }
}

void TestCrossEncoderReranker::testMaxCandidatesCapping()
{
    bs::CrossEncoderReranker reranker(nullptr);
    // Don't initialize — model unavailable

    std::vector<bs::SearchResult> results;
    for (int i = 0; i < 100; ++i) {
        bs::SearchResult sr;
        sr.itemId = i + 1;
        sr.path = QStringLiteral("/home/user/file_%1.txt").arg(i);
        sr.name = QStringLiteral("file_%1.txt").arg(i);
        sr.score = 200.0 - i;
        results.push_back(sr);
    }

    bs::RerankerConfig config;
    config.maxCandidates = 10;

    // With unavailable model, rerank returns 0 regardless of config
    const int boosted = reranker.rerank(QStringLiteral("query"), results, config);
    QCOMPARE(boosted, 0);
    QCOMPARE(static_cast<int>(results.size()), 100); // results not truncated
}

QTEST_MAIN(TestCrossEncoderReranker)
#include "test_cross_encoder_reranker.moc"
