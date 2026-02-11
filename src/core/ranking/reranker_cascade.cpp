#include "core/ranking/reranker_cascade.h"

#include <QElapsedTimer>

#include <algorithm>

namespace bs {

bool RerankerCascade::isAmbiguousTopK(const std::vector<SearchResult>& results,
                                      float marginThreshold)
{
    if (results.size() < 2) {
        return false;
    }

    const double margin = results[0].score - results[1].score;
    if (margin < static_cast<double>(marginThreshold)) {
        return true;
    }

    const int topK = std::min(static_cast<int>(results.size()), 10);
    int highSemantic = 0;
    int lowSemantic = 0;
    for (int i = 0; i < topK; ++i) {
        const double semantic = results[static_cast<size_t>(i)].semanticNormalized;
        if (semantic >= 0.55) {
            ++highSemantic;
        } else if (semantic <= 0.12) {
            ++lowSemantic;
        }
    }
    return highSemantic >= 3 && lowSemantic >= 3;
}

RerankerCascadeStats RerankerCascade::run(const QString& query,
                                          std::vector<SearchResult>& results,
                                          CrossEncoderReranker* stage1,
                                          CrossEncoderReranker* stage2,
                                          const RerankerCascadeConfig& config,
                                          int elapsedBeforeCascadeMs)
{
    RerankerCascadeStats stats;
    if (!config.enabled || results.empty()) {
        return stats;
    }

    QElapsedTimer timer;
    timer.start();

    if (stage1 && stage1->isAvailable() && elapsedBeforeCascadeMs < config.rerankBudgetMs) {
        RerankerConfig stage1Config;
        stage1Config.weight = config.stage1Weight;
        stage1Config.maxCandidates = std::min(config.stage1MaxCandidates,
                                              static_cast<int>(results.size()));
        stage1Config.minScoreThreshold = 0.05f;
        stats.stage1Depth = stage1Config.maxCandidates;
        stage1->rerank(query, results, stage1Config);
        stats.stage1Applied = stage1Config.maxCandidates > 0;
    }

    const int elapsedSoFarMs = elapsedBeforeCascadeMs + static_cast<int>(timer.elapsed());
    if (elapsedSoFarMs >= config.rerankBudgetMs) {
        stats.elapsedMs = static_cast<int>(timer.elapsed());
        return stats;
    }

    stats.ambiguous = isAmbiguousTopK(results, config.ambiguityMarginThreshold);
    if (stats.ambiguous && stage2 && stage2->isAvailable()) {
        RerankerConfig stage2Config;
        stage2Config.weight = config.stage2Weight;
        stage2Config.maxCandidates = std::min(config.stage2MaxCandidates,
                                              static_cast<int>(results.size()));
        stage2Config.minScoreThreshold = 0.10f;
        stats.stage2Depth = stage2Config.maxCandidates;
        stage2->rerank(query, results, stage2Config);
        stats.stage2Applied = stage2Config.maxCandidates > 0;
    }

    stats.elapsedMs = static_cast<int>(timer.elapsed());
    return stats;
}

} // namespace bs
