#pragma once

#include "core/shared/search_result.h"
#include "core/shared/scoring_types.h"
#include "core/ranking/match_classifier.h"
#include "core/ranking/context_signals.h"

#include <QString>
#include <vector>

namespace bs {

class Scorer {
public:
    explicit Scorer(const ScoringWeights& weights = {});

    // Score a single result with full context.
    // bm25RawScore is used only for Content match types.
    ScoreBreakdown computeScore(const SearchResult& result,
                                const QueryContext& context,
                                double bm25RawScore = 0.0) const;

    // Apply scores to a list of results and sort by
    // (finalScore DESC, itemId ASC) for stable tie-breaking.
    void rankResults(std::vector<SearchResult>& results,
                     const QueryContext& context) const;

    // Recency boost: recencyWeight * exp(-timeSinceModification / (decayDays * 86400))
    double computeRecencyBoost(double modifiedAtEpoch) const;

    // Frequency boost: tiered lookup with recency modifier.
    double computeFrequencyBoost(int openCount, double lastOpenEpoch = 0.0) const;

    // Junk penalty: returns junkPenaltyWeight if path contains a known junk pattern.
    double computeJunkPenalty(const QString& filePath) const;

    // Pinned boost: returns pinnedBoostWeight if the item is pinned.
    double computePinnedBoost(bool isPinned) const;

    const ScoringWeights& weights() const { return m_weights; }

private:
    ScoringWeights m_weights;
    ContextSignals m_contextSignals;

    // Known junk directory patterns (node_modules, .build, __pycache__, etc.)
    static const std::vector<QString>& junkPatterns();

    // Compute the base match score from match type and BM25
    double computeBaseMatchScore(MatchType matchType, double bm25RawScore) const;
};

} // namespace bs
