#pragma once

#include "core/shared/search_result.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bs {

class ModelRegistry;

namespace cross_encoder_detail {

std::optional<std::vector<size_t>> candidateLogitOffsets(const std::vector<int64_t>& shape,
                                                         int candidateCount,
                                                         size_t elementCount);
std::optional<float> sigmoidScoreFromLogit(float logit);

} // namespace cross_encoder_detail

struct RerankerConfig {
    float weight = 35.0f;           // Soft boost weight (additive)
    int maxCandidates = 40;         // Max results to score (top-N by existing score)
    float minScoreThreshold = 0.1f; // Skip boost if sigmoid < this
};

class CrossEncoderReranker {
public:
    explicit CrossEncoderReranker(ModelRegistry* registry,
                                  std::string role = "cross-encoder");
    ~CrossEncoderReranker();

    CrossEncoderReranker(const CrossEncoderReranker&) = delete;
    CrossEncoderReranker& operator=(const CrossEncoderReranker&) = delete;

    bool initialize();
    bool isAvailable() const;

    // Score and boost results in-place by adding crossEncoderBoost.
    // Returns count of results that received a boost.
    int rerank(const QString& query, std::vector<SearchResult>& results,
               const RerankerConfig& config = {}) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_role;
};

} // namespace bs
