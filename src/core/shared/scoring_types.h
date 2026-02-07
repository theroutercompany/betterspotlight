#pragma once

namespace bs {

// All 16 configurable scoring weights (doc 06)
struct ScoringWeights {
    // Match type weights
    int exactNameWeight = 200;
    int prefixNameWeight = 150;
    int containsNameWeight = 100;
    int exactPathWeight = 90;
    int prefixPathWeight = 80;
    double contentMatchWeight = 1.0;
    int fuzzyMatchWeight = 30;

    // Boost weights
    int recencyWeight = 30;
    int recencyDecayDays = 7;
    int frequencyTier1Boost = 10;
    int frequencyTier2Boost = 20;
    int frequencyTier3Boost = 30;
    int cwdBoostWeight = 25;
    int appContextBoostWeight = 15;   // M2 only
    int semanticWeight = 40;          // M2 only
    double semanticSimilarityThreshold = 0.7; // M2 only
    int pinnedBoostWeight = 200;
    int junkPenaltyWeight = 50;
};

} // namespace bs
