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
    double contentMatchWeight = 0.6;
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

    // Wave 2: cross-encoder + structured query signal weights
    float crossEncoderWeight = 35.0f;
    float temporalBoostWeight = 12.0f;
    float temporalNearWeight = 6.0f;
    float docTypeIntentWeight = 10.0f;
    float entityMatchWeight = 8.0f;
    float entityMatchCap = 16.0f;
};

} // namespace bs
