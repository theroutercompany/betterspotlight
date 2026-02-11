#pragma once

#include "core/ranking/cross_encoder_reranker.h"
#include "core/shared/search_result.h"

#include <QString>

#include <vector>

namespace bs {

struct RerankerCascadeConfig {
    bool enabled = true;
    int stage1MaxCandidates = 40;
    int stage2MaxCandidates = 12;
    int rerankBudgetMs = 120;
    float stage1Weight = 18.0f;
    float stage2Weight = 35.0f;
    float ambiguityMarginThreshold = 0.08f;
};

struct RerankerCascadeStats {
    bool stage1Applied = false;
    bool stage2Applied = false;
    bool ambiguous = false;
    int stage1Depth = 0;
    int stage2Depth = 0;
    int elapsedMs = 0;
};

class RerankerCascade {
public:
    static RerankerCascadeStats run(const QString& query,
                                    std::vector<SearchResult>& results,
                                    CrossEncoderReranker* stage1,
                                    CrossEncoderReranker* stage2,
                                    const RerankerCascadeConfig& config,
                                    int elapsedBeforeCascadeMs);

private:
    static bool isAmbiguousTopK(const std::vector<SearchResult>& results,
                                float marginThreshold);
};

} // namespace bs

