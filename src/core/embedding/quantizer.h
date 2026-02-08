#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

struct QuantizedVector {
    float scale = 0.0F;
    int8_t zeroPoint = 0;
    std::vector<int8_t> data;
};

class Quantizer {
public:
    static constexpr int kEmbeddingDimensions = 384;

    QuantizedVector quantize(const std::vector<float>& embedding) const;
    std::vector<float> dequantize(const QuantizedVector& qv) const;

    static constexpr std::size_t serializedSize()
    {
        return sizeof(float) + sizeof(int8_t) + kEmbeddingDimensions;
    }

    std::vector<uint8_t> serialize(const QuantizedVector& qv) const;
    bool deserialize(const std::vector<uint8_t>& buffer, QuantizedVector* out) const;
};

} // namespace bs
