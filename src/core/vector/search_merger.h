#pragma once

#include "core/shared/search_result.h"

#include <cstdint>
#include <vector>

namespace bs {

class Scorer;

struct SemanticResult {
    int64_t itemId = 0;
    float cosineSimilarity = 0.0f;
};

struct MergeConfig {
    float lexicalWeight = 0.6f;
    float semanticWeight = 0.4f;
    float similarityThreshold = 0.7f;
    int rrfK = 60;
    int maxResults = 20;
    float semanticSoftmaxTemperature = 8.0f;
    int semanticPassageCap = 3;
};

enum class MergeCategory {
    Both,
    LexicalOnly,
    SemanticOnly,
};

class SearchMerger {
public:
    static std::vector<SearchResult> merge(
        const std::vector<SearchResult>& lexicalResults,
        const std::vector<SemanticResult>& semanticResults,
        MergeConfig config = {});

    static float normalizeLexicalScore(float score, float maxScore);
    static float normalizeSemanticScore(float cosineSim, float threshold);
    static float aggregateSemanticScore(const std::vector<float>& similarities,
                                        const MergeConfig& config);
};

} // namespace bs
