#include <QtTest/QtTest>

#include "services/query/rerank_score_utils.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <limits>

class TestRerankScoreUtils : public QObject {
    Q_OBJECT

private slots:
    void testParseRerankScoresPreservesFiniteScores();
    void testParseRerankScoresRejectsMalformedRows();
    void testParseRerankScoresRejectsDuplicateItems();
};

void TestRerankScoreUtils::testParseRerankScoresPreservesFiniteScores()
{
    QString reason;
    QJsonArray scores;

    QJsonObject first;
    first[QStringLiteral("itemId")] = 11;
    first[QStringLiteral("score")] = 0.75;
    scores.append(first);

    QJsonObject second;
    second[QStringLiteral("itemId")] = 12;
    second[QStringLiteral("score")] = 0.125;
    scores.append(second);

    const std::optional<QHash<qint64, float>> parsed =
        bs::query_rerank::parseRerankScores(scores, &reason);

    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->size(), 2);
    QVERIFY(std::abs(parsed->value(11) - 0.75F) < 0.0001F);
    QVERIFY(std::abs(parsed->value(12) - 0.125F) < 0.0001F);
    QVERIFY(reason.isEmpty());
}

void TestRerankScoreUtils::testParseRerankScoresRejectsMalformedRows()
{
    QString reason;

    QJsonArray scores;
    scores.append(QStringLiteral("not an object"));
    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_row_invalid"));

    reason.clear();
    scores = QJsonArray{};
    QJsonObject fractionalId;
    fractionalId[QStringLiteral("itemId")] = 1.5;
    fractionalId[QStringLiteral("score")] = 0.4;
    scores.append(fractionalId);
    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_item_id_invalid"));

    reason.clear();
    scores = QJsonArray{};
    QJsonObject unsafeId;
    unsafeId[QStringLiteral("itemId")] = 9007199254740992.0;
    unsafeId[QStringLiteral("score")] = 0.4;
    scores.append(unsafeId);
    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_item_id_invalid"));

    reason.clear();
    scores = QJsonArray{};
    QJsonObject missingScore;
    missingScore[QStringLiteral("itemId")] = 1;
    scores.append(missingScore);
    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_invalid"));

    reason.clear();
    scores = QJsonArray{};
    QJsonObject nonFiniteScore;
    nonFiniteScore[QStringLiteral("itemId")] = 1;
    nonFiniteScore[QStringLiteral("score")] = std::numeric_limits<double>::infinity();
    scores.append(nonFiniteScore);
    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_invalid"));

    reason.clear();
    scores = QJsonArray{};
    QJsonObject outOfRangeScore;
    outOfRangeScore[QStringLiteral("itemId")] = 1;
    outOfRangeScore[QStringLiteral("score")] = 1.1;
    scores.append(outOfRangeScore);
    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_out_of_range"));
}

void TestRerankScoreUtils::testParseRerankScoresRejectsDuplicateItems()
{
    QString reason;
    QJsonArray scores;

    QJsonObject first;
    first[QStringLiteral("itemId")] = 5;
    first[QStringLiteral("score")] = 0.1;
    scores.append(first);

    QJsonObject second;
    second[QStringLiteral("itemId")] = 5;
    second[QStringLiteral("score")] = 0.9;
    scores.append(second);

    QVERIFY(!bs::query_rerank::parseRerankScores(scores, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_duplicate_item"));
}

QTEST_MAIN(TestRerankScoreUtils)
#include "test_rerank_score_utils.moc"
