#include <QtTest/QtTest>

#include "core/ranking/reranker_cascade.h"

class TestRerankerCascade : public QObject {
    Q_OBJECT

private slots:
    void testDisabledCascadeNoOps();
    void testAmbiguityDetectionByMargin();
};

void TestRerankerCascade::testDisabledCascadeNoOps()
{
    std::vector<bs::SearchResult> results(3);
    for (int i = 0; i < 3; ++i) {
        results[static_cast<size_t>(i)].itemId = i + 1;
        results[static_cast<size_t>(i)].score = 100.0 - i;
    }

    bs::RerankerCascadeConfig config;
    config.enabled = false;
    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("test"),
        results,
        nullptr,
        nullptr,
        config,
        0);
    QVERIFY(!stats.stage1Applied);
    QVERIFY(!stats.stage2Applied);
    QVERIFY(!stats.ambiguous);
}

void TestRerankerCascade::testAmbiguityDetectionByMargin()
{
    std::vector<bs::SearchResult> results(3);
    results[0].itemId = 1;
    results[1].itemId = 2;
    results[2].itemId = 3;
    results[0].score = 100.00;
    results[1].score = 99.96;
    results[2].score = 99.10;

    bs::RerankerCascadeConfig config;
    config.enabled = true;
    config.ambiguityMarginThreshold = 0.08f;
    config.rerankBudgetMs = 200;
    const bs::RerankerCascadeStats stats = bs::RerankerCascade::run(
        QStringLiteral("ambiguous query"),
        results,
        nullptr,
        nullptr,
        config,
        20);

    QVERIFY(stats.ambiguous);
    QVERIFY(!stats.stage1Applied);
    QVERIFY(!stats.stage2Applied);
}

QTEST_MAIN(TestRerankerCascade)
#include "test_reranker_cascade.moc"

