#include "core/embedding/quantizer.h"

#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace bs {

QuantizedVector Quantizer::quantize(const std::vector<float>& embedding) const
{
    QuantizedVector qv;

    if (embedding.size() != static_cast<size_t>(kEmbeddingDimensions)) {
        qWarning() << "Quantizer::quantize expected 384 values, got" << embedding.size();
        return qv;
    }

    qv.data.assign(kEmbeddingDimensions, 0);

    const auto [minIt, maxIt] = std::minmax_element(embedding.begin(), embedding.end());
    const float minValue = *minIt;
    const float maxValue = *maxIt;

    float scale = (maxValue - minValue) / 255.0F;
    if (scale <= std::numeric_limits<float>::epsilon()) {
        scale = 1.0F;
    }

    const int zeroPointInt = static_cast<int>(std::lround(-minValue / scale));
    const int clampedZeroPoint = std::clamp(zeroPointInt, 0, 255);

    qv.scale = scale;
    qv.zeroPoint = static_cast<int8_t>(clampedZeroPoint);

    for (int i = 0; i < kEmbeddingDimensions; ++i) {
        const int uint8Value = std::clamp(
            static_cast<int>(std::lround(embedding[static_cast<size_t>(i)] / scale)) + clampedZeroPoint,
            0,
            255);
        qv.data[static_cast<size_t>(i)] = static_cast<int8_t>(uint8Value - 128);
    }

    return qv;
}

std::vector<float> Quantizer::dequantize(const QuantizedVector& qv) const
{
    std::vector<float> output;
    if (qv.data.size() != static_cast<size_t>(kEmbeddingDimensions)) {
        qWarning() << "Quantizer::dequantize expected 384 int8 values, got" << qv.data.size();
        return output;
    }

    output.resize(static_cast<size_t>(kEmbeddingDimensions));
    const int zeroPoint = static_cast<uint8_t>(qv.zeroPoint);
    for (int i = 0; i < kEmbeddingDimensions; ++i) {
        const uint8_t uint8Value = static_cast<uint8_t>(static_cast<int>(qv.data[static_cast<size_t>(i)]) + 128);
        output[static_cast<size_t>(i)] = static_cast<float>(
            (static_cast<int>(uint8Value) - zeroPoint) * qv.scale);
    }
    return output;
}

std::vector<uint8_t> Quantizer::serialize(const QuantizedVector& qv) const
{
    std::vector<uint8_t> buffer;
    if (qv.data.size() != static_cast<size_t>(kEmbeddingDimensions)) {
        qWarning() << "Quantizer::serialize expected 384 int8 values, got" << qv.data.size();
        return buffer;
    }

    buffer.resize(serializedSize());
    std::memcpy(buffer.data(), &qv.scale, sizeof(float));
    std::memcpy(buffer.data() + sizeof(float), &qv.zeroPoint, sizeof(int8_t));
    std::memcpy(buffer.data() + sizeof(float) + sizeof(int8_t), qv.data.data(), kEmbeddingDimensions);
    return buffer;
}

bool Quantizer::deserialize(const std::vector<uint8_t>& buffer, QuantizedVector* out) const
{
    if (!out || buffer.size() != serializedSize()) {
        return false;
    }

    QuantizedVector decoded;
    decoded.data.resize(kEmbeddingDimensions);

    std::memcpy(&decoded.scale, buffer.data(), sizeof(float));
    std::memcpy(&decoded.zeroPoint, buffer.data() + sizeof(float), sizeof(int8_t));
    std::memcpy(decoded.data.data(),
                buffer.data() + sizeof(float) + sizeof(int8_t),
                kEmbeddingDimensions);

    *out = std::move(decoded);
    return true;
}

} // namespace bs
