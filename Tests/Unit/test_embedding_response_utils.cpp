#include <QtTest/QtTest>

#include "services/query/embedding_response_utils.h"

#include <QJsonArray>

#include <cmath>
#include <limits>

class TestEmbeddingResponseUtils : public QObject {
    Q_OBJECT

private slots:
    void testParseEmbeddingVectorPreservesFiniteValues();
    void testParseEmbeddingVectorRejectsInvalidDimensionsAndValues();
    void testParseEmbeddingBatchRejectsMalformedRows();
};

void TestEmbeddingResponseUtils::testParseEmbeddingVectorPreservesFiniteValues()
{
    QString reason;
    QJsonArray values;
    values.append(0.5);
    values.append(-0.25);
    values.append(0.0);

    const std::optional<std::vector<float>> parsed =
        bs::query_embedding::parseEmbeddingVector(values, 3, &reason);

    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->size(), size_t{3});
    QVERIFY(std::abs(parsed->at(0) - 0.5F) < 0.0001F);
    QVERIFY(std::abs(parsed->at(1) - -0.25F) < 0.0001F);
    QCOMPARE(parsed->at(2), 0.0F);
    QVERIFY(reason.isEmpty());
}

void TestEmbeddingResponseUtils::testParseEmbeddingVectorRejectsInvalidDimensionsAndValues()
{
    QString reason;
    QJsonArray values;
    values.append(0.5);
    values.append(0.25);

    QVERIFY(!bs::query_embedding::parseEmbeddingVector(values, 0, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_dimension_invalid"));

    reason.clear();
    QVERIFY(!bs::query_embedding::parseEmbeddingVector(values, 3, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_dimension_mismatch"));

    reason.clear();
    QJsonArray nonNumeric;
    nonNumeric.append(0.5);
    nonNumeric.append(QStringLiteral("0.25"));
    QVERIFY(!bs::query_embedding::parseEmbeddingVector(nonNumeric, 2, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_value_invalid"));

    reason.clear();
    QJsonArray nonFinite;
    nonFinite.append(0.5);
    nonFinite.append(std::numeric_limits<double>::infinity());
    QVERIFY(!bs::query_embedding::parseEmbeddingVector(nonFinite, 2, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_value_invalid"));
}

void TestEmbeddingResponseUtils::testParseEmbeddingBatchRejectsMalformedRows()
{
    QString reason;

    QJsonArray rows;
    QJsonArray first;
    first.append(1.0);
    first.append(0.0);
    rows.append(first);

    const std::optional<std::vector<std::vector<float>>> parsed =
        bs::query_embedding::parseEmbeddingBatch(rows, 1, 2, &reason);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->size(), size_t{1});
    QCOMPARE(parsed->front().size(), size_t{2});

    reason.clear();
    QVERIFY(!bs::query_embedding::parseEmbeddingBatch(rows, 2, 2, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_row_count_mismatch"));

    reason.clear();
    QJsonArray malformedRows;
    malformedRows.append(QStringLiteral("not an array"));
    QVERIFY(!bs::query_embedding::parseEmbeddingBatch(malformedRows, 1, 2, &reason).has_value());
    QCOMPARE(reason, QStringLiteral("embedding_row_invalid"));

    reason.clear();
    QJsonArray wrongDimensionRows;
    QJsonArray shortRow;
    shortRow.append(1.0);
    wrongDimensionRows.append(shortRow);
    QVERIFY(!bs::query_embedding::parseEmbeddingBatch(wrongDimensionRows, 1, 2, &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("embedding_dimension_mismatch"));
}

QTEST_MAIN(TestEmbeddingResponseUtils)
#include "test_embedding_response_utils.moc"
