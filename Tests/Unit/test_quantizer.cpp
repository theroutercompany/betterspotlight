#include <QtTest/QtTest>
#include "core/embedding/quantizer.h"

#include <cmath>

class TestQuantizer : public QObject {
    Q_OBJECT

private slots:
    void testQuantizeReturnsCorrectSize();
    void testQuantizeUniformVector();
    void testQuantizeNormalVector();
    void testDequantizeRoundtrip();
    void testQuantizeZeroVector();
    void testSerializeDeserializeRoundtrip();
    void testSerializedSize();
    void testDeserializeInvalidBuffer();
    void testEmptyInput();
};

void TestQuantizer::testQuantizeReturnsCorrectSize()
{
    bs::Quantizer quantizer;
    std::vector<float> embedding(static_cast<size_t>(bs::Quantizer::kEmbeddingDimensions), 0.5F);

    const bs::QuantizedVector qv = quantizer.quantize(embedding);
    QCOMPARE(static_cast<int>(qv.data.size()), bs::Quantizer::kEmbeddingDimensions);
}

void TestQuantizer::testQuantizeUniformVector()
{
    bs::Quantizer quantizer;
    std::vector<float> uniform(static_cast<size_t>(bs::Quantizer::kEmbeddingDimensions), 0.5F);

    const bs::QuantizedVector qv = quantizer.quantize(uniform);
    QCOMPARE(static_cast<int>(qv.data.size()), bs::Quantizer::kEmbeddingDimensions);

    int8_t first = qv.data[0];
    for (size_t i = 1; i < qv.data.size(); ++i) {
        QCOMPARE(qv.data[i], first);
    }
}

void TestQuantizer::testQuantizeNormalVector()
{
    bs::Quantizer quantizer;
    std::vector<float> vec(static_cast<size_t>(bs::Quantizer::kEmbeddingDimensions));
    for (int i = 0; i < bs::Quantizer::kEmbeddingDimensions; ++i) {
        vec[static_cast<size_t>(i)] = std::sin(static_cast<float>(i) * 0.1F) * 0.5F;
    }

    const bs::QuantizedVector qv = quantizer.quantize(vec);
    const std::vector<float> recovered = quantizer.dequantize(qv);

    double dotProduct = 0.0;
    double normA = 0.0;
    double normB = 0.0;
    for (int i = 0; i < bs::Quantizer::kEmbeddingDimensions; ++i) {
        const auto a = static_cast<double>(vec[static_cast<size_t>(i)]);
        const auto b = static_cast<double>(recovered[static_cast<size_t>(i)]);
        dotProduct += a * b;
        normA += a * a;
        normB += b * b;
    }
    const double cosineSim = dotProduct / (std::sqrt(normA) * std::sqrt(normB));
    QVERIFY2(cosineSim > 0.95, qPrintable(QString("Cosine similarity: %1").arg(cosineSim)));
}

void TestQuantizer::testDequantizeRoundtrip()
{
    bs::Quantizer quantizer;
    std::vector<float> embedding(static_cast<size_t>(bs::Quantizer::kEmbeddingDimensions));
    for (int i = 0; i < bs::Quantizer::kEmbeddingDimensions; ++i) {
        embedding[static_cast<size_t>(i)] = static_cast<float>((i % 17) / 16.0);
    }

    const bs::QuantizedVector qv = quantizer.quantize(embedding);
    const std::vector<float> restored = quantizer.dequantize(qv);
    QCOMPARE(static_cast<int>(restored.size()), bs::Quantizer::kEmbeddingDimensions);

    for (int i = 0; i < bs::Quantizer::kEmbeddingDimensions; ++i) {
        QVERIFY(std::fabs(restored[static_cast<size_t>(i)] - embedding[static_cast<size_t>(i)]) < 0.02F);
    }
}

void TestQuantizer::testQuantizeZeroVector()
{
    bs::Quantizer quantizer;
    std::vector<float> zero(static_cast<size_t>(bs::Quantizer::kEmbeddingDimensions), 0.0F);

    const bs::QuantizedVector qv = quantizer.quantize(zero);
    QCOMPARE(static_cast<int>(qv.data.size()), bs::Quantizer::kEmbeddingDimensions);
    QVERIFY(qv.scale >= 0.0F);

    const std::vector<float> restored = quantizer.dequantize(qv);
    QVERIFY(!restored.empty());
    for (float value : restored) {
        QVERIFY(std::fabs(value) < 0.001F);
    }
}

void TestQuantizer::testSerializeDeserializeRoundtrip()
{
    bs::Quantizer quantizer;
    std::vector<float> embedding(static_cast<size_t>(bs::Quantizer::kEmbeddingDimensions), 0.25F);
    const bs::QuantizedVector qv = quantizer.quantize(embedding);

    const std::vector<uint8_t> buffer = quantizer.serialize(qv);
    QVERIFY(!buffer.empty());

    bs::QuantizedVector decoded;
    QVERIFY(quantizer.deserialize(buffer, &decoded));
    QCOMPARE(decoded.scale, qv.scale);
    QCOMPARE(decoded.zeroPoint, qv.zeroPoint);
    QCOMPARE(static_cast<int>(decoded.data.size()), static_cast<int>(qv.data.size()));
    QCOMPARE(decoded.data[0], qv.data[0]);
}

void TestQuantizer::testSerializedSize()
{
    const std::size_t expected = sizeof(float) + sizeof(int8_t) + 384;
    QCOMPARE(bs::Quantizer::serializedSize(), expected);
}

void TestQuantizer::testDeserializeInvalidBuffer()
{
    bs::Quantizer quantizer;
    std::vector<uint8_t> shortBuffer(10U, 0);
    bs::QuantizedVector out;
    QVERIFY(!quantizer.deserialize(shortBuffer, &out));
}

void TestQuantizer::testEmptyInput()
{
    bs::Quantizer quantizer;
    std::vector<float> empty;

    const bs::QuantizedVector qv = quantizer.quantize(empty);
    QVERIFY(qv.data.empty());
}

QTEST_MAIN(TestQuantizer)
#include "test_quantizer.moc"
