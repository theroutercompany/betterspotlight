#include "core/vector/search_merger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

double getLexicalRawScore(const bs::SearchResult& result)
{
    return result.score;
}

void setMergedScore(bs::SearchResult& result, double score)
{
    result.score = score;
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
    lexicalById.reserve(lexicalResults.size());
    semanticById.reserve(semanticResults.size());

    float maxLexicalScore = 0.0f;
    for (const SearchResult& result : lexicalResults) {
        lexicalById[result.itemId] = &result;
        maxLexicalScore = std::max(maxLexicalScore,
                                   static_cast<float>(getLexicalRawScore(result)));
    }

    for (const SemanticResult& result : semanticResults) {
        semanticById[result.itemId] = result.cosineSimilarity;
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

        const float normalizedLexical =
            lexicalIt != lexicalById.end()
                ? normalizeLexicalScore(
                      static_cast<float>(getLexicalRawScore(*lexicalIt->second)),
                      maxLexicalScore)
                : 0.0f;

        const float normalizedSemantic =
            semanticIt != semanticById.end()
                ? normalizeSemanticScore(semanticIt->second, config.similarityThreshold)
                : 0.0f;

        float mergedScore = 0.0f;
        switch (category) {
        case MergeCategory::Both:
            mergedScore = (config.lexicalWeight * normalizedLexical)
                + (config.semanticWeight * normalizedSemantic);
            break;
        case MergeCategory::LexicalOnly:
            mergedScore = config.lexicalWeight * normalizedLexical;
            break;
        case MergeCategory::SemanticOnly:
            mergedScore = config.semanticWeight * normalizedSemantic;
            break;
        }

        if (category == MergeCategory::SemanticOnly) {
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
                         const double lhsScore = getLexicalRawScore(lhs);
                         const double rhsScore = getLexicalRawScore(rhs);

                         if (std::fabs(lhsScore - rhsScore) > 0.0) {
                             return lhsScore > rhsScore;
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
