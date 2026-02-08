#include "core/vector/search_merger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

void setMergedScore(bs::SearchResult& result, double score)
{
    result.score = score;
}

double computeRrfContribution(float weight, int rank, int rrfK)
{
    if (rank <= 0) {
        return 0.0;
    }
    const int denom = std::max(1, rrfK) + rank;
    return static_cast<double>(weight) / static_cast<double>(denom);
}

} // namespace

namespace bs {

float SearchMerger::normalizeLexicalScore(float score, float maxScore)
{
    if (maxScore <= 0.0f) {
        return 0.0f;
    }
    return score / maxScore;
}

float SearchMerger::normalizeSemanticScore(float cosineSim, float threshold)
{
    const float denominator = 1.0f - threshold;
    if (denominator <= 0.0f) {
        return cosineSim >= 1.0f ? 1.0f : 0.0f;
    }

    const float normalized = (cosineSim - threshold) / denominator;
    return std::clamp(normalized, 0.0f, 1.0f);
}

std::vector<SearchResult> SearchMerger::merge(
    const std::vector<SearchResult>& lexicalResults,
    const std::vector<SemanticResult>& semanticResults,
    MergeConfig config)
{
    std::unordered_map<int64_t, const SearchResult*> lexicalById;
    std::unordered_map<int64_t, float> semanticById;
    std::unordered_map<int64_t, int> lexicalRankById;
    std::unordered_map<int64_t, int> semanticRankById;
    lexicalById.reserve(lexicalResults.size());
    semanticById.reserve(semanticResults.size());
    lexicalRankById.reserve(lexicalResults.size());
    semanticRankById.reserve(semanticResults.size());

    for (size_t i = 0; i < lexicalResults.size(); ++i) {
        const SearchResult& result = lexicalResults[i];
        lexicalById[result.itemId] = &result;
        lexicalRankById[result.itemId] = static_cast<int>(i) + 1;
    }

    for (size_t i = 0; i < semanticResults.size(); ++i) {
        const SemanticResult& result = semanticResults[i];
        semanticById[result.itemId] = result.cosineSimilarity;
        semanticRankById[result.itemId] = static_cast<int>(i) + 1;
    }

    std::unordered_map<int64_t, MergeCategory> categories;
    categories.reserve(lexicalById.size() + semanticById.size());
    for (const auto& [itemId, _] : lexicalById) {
        categories[itemId] = MergeCategory::LexicalOnly;
    }
    for (const auto& [itemId, _] : semanticById) {
        auto it = categories.find(itemId);
        if (it == categories.end()) {
            categories[itemId] = MergeCategory::SemanticOnly;
        } else {
            it->second = MergeCategory::Both;
        }
    }

    std::vector<SearchResult> mergedResults;
    mergedResults.reserve(categories.size());

    for (const auto& [itemId, category] : categories) {
        const auto lexicalIt = lexicalById.find(itemId);
        const auto semanticIt = semanticById.find(itemId);

        const int lexicalRank =
            lexicalIt != lexicalById.end() ? lexicalRankById[itemId] : 0;
        const int semanticRank =
            semanticIt != semanticById.end() ? semanticRankById[itemId] : 0;

        const double lexicalContribution =
            computeRrfContribution(config.lexicalWeight, lexicalRank, config.rrfK);
        const double semanticContribution =
            computeRrfContribution(config.semanticWeight, semanticRank, config.rrfK);
        const double mergedScore = lexicalContribution + semanticContribution;

        if (category == MergeCategory::SemanticOnly) {
            const float normalizedSemantic = normalizeSemanticScore(
                semanticIt->second, config.similarityThreshold);
            if (normalizedSemantic <= 0.0f || mergedScore <= 0.0) {
                continue;
            }
            SearchResult result;
            result.itemId = itemId;
            result.matchType = MatchType::Content;
            setMergedScore(result, mergedScore);
            mergedResults.push_back(std::move(result));
            continue;
        }

        SearchResult result = *lexicalIt->second;
        setMergedScore(result, mergedScore);
        mergedResults.push_back(std::move(result));
    }

    std::stable_sort(mergedResults.begin(), mergedResults.end(),
                     [](const SearchResult& lhs, const SearchResult& rhs) {
                         if (lhs.score != rhs.score) {
                             return lhs.score > rhs.score;
                         }
                         return lhs.itemId < rhs.itemId;
                     });

    const int maxResults = std::max(config.maxResults, 0);
    const std::size_t limit = static_cast<std::size_t>(maxResults);
    if (mergedResults.size() > limit) {
        mergedResults.resize(limit);
    }

    return mergedResults;
}

} // namespace bs
