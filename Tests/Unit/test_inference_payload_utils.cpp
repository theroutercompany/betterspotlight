#include <QtTest/QtTest>

#include "services/inference/inference_payload_utils.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <limits>
#include <vector>

class TestInferencePayloadUtils : public QObject {
    Q_OBJECT

private slots:
    void testParseTextBatchPreservesStringCardinality();
    void testParseTextBatchRejectsNonStringRows();
    void testSerializeEmbeddingRejectsEmptyAndNonFiniteRows();
    void testSerializeEmbeddingBatchRejectsCardinalityAndRowFailures();
    void testSerializeEmbeddingBatchNormalizesFiniteRows();
    void testParseRerankCandidatesRejectsMalformedNumericFields();
    void testParseRerankCandidatesPreservesSearchResultFields();
    void testSerializeRerankScoresRejectsNonFiniteScores();
};

void TestInferencePayloadUtils::testParseTextBatchPreservesStringCardinality()
{
    QString reason;
    QJsonArray texts;
    texts.append(QStringLiteral(" alpha "));
    texts.append(QStringLiteral(" "));
    texts.append(QStringLiteral(""));
    texts.append(QStringLiteral("beta"));

    const std::optional<std::vector<QString>> parsed =
        bs::inference_payload::parseTextBatch(texts, &reason);

    QVERIFY(parsed.has_value());
    QCOMPARE(static_cast<int>(parsed->size()), 4);
    QCOMPARE(parsed->at(0), QStringLiteral(" alpha "));
    QCOMPARE(parsed->at(1), QStringLiteral(" "));
    QCOMPARE(parsed->at(2), QStringLiteral(""));
    QCOMPARE(parsed->at(3), QStringLiteral("beta"));
    QVERIFY(reason.isEmpty());
}

void TestInferencePayloadUtils::testParseTextBatchRejectsNonStringRows()
{
    QString reason;
    QJsonArray texts;
    texts.append(QStringLiteral("alpha"));
    texts.append(42);

    QVERIFY(!bs::inference_payload::parseTextBatch(texts, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("text_not_string"));
}

void TestInferencePayloadUtils::testSerializeEmbeddingRejectsEmptyAndNonFiniteRows()
{
    QString reason;

    QVERIFY(!bs::inference_payload::serializeEmbedding({}, false, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_empty"));

    reason.clear();
    QVERIFY(!bs::inference_payload::serializeEmbedding({
        1.0F,
        std::numeric_limits<float>::quiet_NaN(),
    }, false, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_non_finite"));

    reason.clear();
    QVERIFY(!bs::inference_payload::serializeEmbedding({
        1.0F,
        std::numeric_limits<float>::infinity(),
    }, true, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_non_finite"));
}

void TestInferencePayloadUtils::testSerializeEmbeddingBatchRejectsCardinalityAndRowFailures()
{
    QString reason;

    QVERIFY(!bs::inference_payload::serializeEmbeddingBatch({{1.0F, 0.0F}},
                                                           2,
                                                           true,
                                                           &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("embedding_size_mismatch"));

    reason.clear();
    QVERIFY(!bs::inference_payload::serializeEmbeddingBatch({{1.0F, 0.0F}, {}},
                                                           2,
                                                           true,
                                                           &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("embedding_empty"));

    reason.clear();
    QVERIFY(!bs::inference_payload::serializeEmbeddingBatch({
                                                               {1.0F, 0.0F},
                                                               {2.0F, std::numeric_limits<float>::quiet_NaN()},
                                                           },
                                                           2,
                                                           true,
                                                           &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("embedding_non_finite"));
}

void TestInferencePayloadUtils::testSerializeEmbeddingBatchNormalizesFiniteRows()
{
    QString reason;
    const std::optional<QJsonArray> serialized =
        bs::inference_payload::serializeEmbeddingBatch({
                                                           {3.0F, 4.0F},
                                                           {0.0F, 0.0F},
                                                       },
                                                       2,
                                                       true,
                                                       &reason);

    QVERIFY(serialized.has_value());
    QCOMPARE(serialized->size(), 2);
    const QJsonArray first = serialized->at(0).toArray();
    QCOMPARE(first.size(), 2);
    QVERIFY(std::abs(first.at(0).toDouble() - 0.6) < 0.0001);
    QVERIFY(std::abs(first.at(1).toDouble() - 0.8) < 0.0001);

    const QJsonArray second = serialized->at(1).toArray();
    QCOMPARE(second.size(), 2);
    QCOMPARE(second.at(0).toDouble(), 0.0);
    QCOMPARE(second.at(1).toDouble(), 0.0);
    QVERIFY(reason.isEmpty());
}

void TestInferencePayloadUtils::testParseRerankCandidatesRejectsMalformedNumericFields()
{
    QString reason;

    QJsonArray candidates;
    candidates.append(QStringLiteral("not an object"));
    QVERIFY(!bs::inference_payload::parseRerankCandidates(candidates, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("candidate_not_object"));

    reason.clear();
    candidates = QJsonArray{};
    QJsonObject fractionalId;
    fractionalId[QStringLiteral("itemId")] = 42.5;
    fractionalId[QStringLiteral("score")] = 0.5;
    candidates.append(fractionalId);
    QVERIFY(!bs::inference_payload::parseRerankCandidates(candidates, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("candidate_item_id_invalid"));

    reason.clear();
    candidates = QJsonArray{};
    QJsonObject unsafeId;
    unsafeId[QStringLiteral("itemId")] = 9007199254740992.0;
    unsafeId[QStringLiteral("score")] = 0.5;
    candidates.append(unsafeId);
    QVERIFY(!bs::inference_payload::parseRerankCandidates(candidates, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("candidate_item_id_invalid"));

    reason.clear();
    candidates = QJsonArray{};
    QJsonObject missingScore;
    missingScore[QStringLiteral("itemId")] = 42;
    candidates.append(missingScore);
    QVERIFY(!bs::inference_payload::parseRerankCandidates(candidates, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("candidate_score_invalid"));

    reason.clear();
    candidates = QJsonArray{};
    QJsonObject nonFiniteScore;
    nonFiniteScore[QStringLiteral("itemId")] = 42;
    nonFiniteScore[QStringLiteral("score")] = std::numeric_limits<double>::quiet_NaN();
    candidates.append(nonFiniteScore);
    QVERIFY(!bs::inference_payload::parseRerankCandidates(candidates, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("candidate_score_invalid"));
}

void TestInferencePayloadUtils::testParseRerankCandidatesPreservesSearchResultFields()
{
    QString reason;
    QJsonObject candidate;
    candidate[QStringLiteral("itemId")] = 42;
    candidate[QStringLiteral("path")] = QStringLiteral("/tmp/rank.md");
    candidate[QStringLiteral("name")] = QStringLiteral("rank.md");
    candidate[QStringLiteral("snippet")] = QStringLiteral("ranking systems");
    candidate[QStringLiteral("score")] = 3.25;

    QJsonArray candidates;
    candidates.append(candidate);

    const std::optional<std::vector<bs::SearchResult>> parsed =
        bs::inference_payload::parseRerankCandidates(candidates, &reason);

    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->size(), size_t{1});
    QCOMPARE(parsed->front().itemId, int64_t{42});
    QCOMPARE(parsed->front().path, QStringLiteral("/tmp/rank.md"));
    QCOMPARE(parsed->front().name, QStringLiteral("rank.md"));
    QCOMPARE(parsed->front().snippet, QStringLiteral("ranking systems"));
    QCOMPARE(parsed->front().score, 3.25);
    QVERIFY(reason.isEmpty());
}

void TestInferencePayloadUtils::testSerializeRerankScoresRejectsNonFiniteScores()
{
    QString reason;

    bs::SearchResult valid;
    valid.itemId = 7;
    valid.crossEncoderScore = 0.875F;
    const std::optional<QJsonArray> serialized =
        bs::inference_payload::serializeRerankScores({valid}, &reason);
    QVERIFY(serialized.has_value());
    QCOMPARE(serialized->size(), 1);
    const QJsonObject score = serialized->at(0).toObject();
    QCOMPARE(score.value(QStringLiteral("itemId")).toInteger(), qint64{7});
    QVERIFY(std::abs(score.value(QStringLiteral("score")).toDouble() - 0.875) < 0.0001);
    QVERIFY(reason.isEmpty());

    bs::SearchResult invalid;
    invalid.itemId = 8;
    invalid.crossEncoderScore = std::numeric_limits<float>::infinity();
    QVERIFY(!bs::inference_payload::serializeRerankScores({invalid}, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_non_finite"));

    reason.clear();
    bs::SearchResult outOfRange;
    outOfRange.itemId = 9;
    outOfRange.crossEncoderScore = 1.1F;
    QVERIFY(!bs::inference_payload::serializeRerankScores({outOfRange}, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("rerank_score_out_of_range"));
}

QTEST_MAIN(TestInferencePayloadUtils)
#include "test_inference_payload_utils.moc"
