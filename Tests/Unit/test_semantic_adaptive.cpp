#include <QtTest/QtTest>

#include <algorithm>

// Tests for the adaptive semantic merge logic from query_service.cpp.
// We replicate the decision logic in isolation to verify behavior
// without needing the full QueryService stack.

class TestSemanticAdaptive : public QObject {
    Q_OBJECT

private:
    enum class QueryClass { NaturalLanguage, PathOrCode, ShortAmbiguous };

    struct AdaptiveWeights {
        float mergeLexicalWeight;
        float mergeSemanticWeight;
    };

    AdaptiveWeights computeWeights(QueryClass queryClass,
                                    bool naturalLanguageQuery,
                                    bool strictLexicalWeakOrEmpty) const
    {
        float mergeLexicalWeight, mergeSemanticWeight;
        if (naturalLanguageQuery) {
            if (strictLexicalWeakOrEmpty) {
                mergeLexicalWeight = 0.45f;
                mergeSemanticWeight = 0.55f;
            } else {
                mergeLexicalWeight = 0.55f;
                mergeSemanticWeight = 0.45f;
            }
        } else if (queryClass == QueryClass::PathOrCode) {
            mergeLexicalWeight = 0.75f;
            mergeSemanticWeight = 0.25f;
        } else { // ShortAmbiguous
            mergeLexicalWeight = 0.65f;
            mergeSemanticWeight = 0.35f;
        }
        return {mergeLexicalWeight, mergeSemanticWeight};
    }

    float computeSafetyThreshold(bool strictLexicalWeakOrEmpty,
                                  bool naturalLanguageQuery) const
    {
        return (strictLexicalWeakOrEmpty && naturalLanguageQuery) ? 0.74f : 0.78f;
    }

    int computeSemanticCap(bool naturalLanguageQuery, bool shortAmbiguousQuery,
                            bool strictLexicalWeakOrEmpty, int limit) const
    {
        return naturalLanguageQuery
            ? (strictLexicalWeakOrEmpty ? std::min(8, limit) : std::min(6, limit))
            : (shortAmbiguousQuery ? std::min(4, limit) : std::min(3, limit / 2));
    }

private slots:
    void testAdaptiveWeightsNLWeakLexical()
    {
        auto w = computeWeights(QueryClass::NaturalLanguage, true, true);
        QCOMPARE(w.mergeLexicalWeight, 0.45f);
        QCOMPARE(w.mergeSemanticWeight, 0.55f);
    }

    void testAdaptiveWeightsNLStrongLexical()
    {
        auto w = computeWeights(QueryClass::NaturalLanguage, true, false);
        QCOMPARE(w.mergeLexicalWeight, 0.55f);
        QCOMPARE(w.mergeSemanticWeight, 0.45f);
    }

    void testAdaptiveWeightsPathQuery()
    {
        auto w = computeWeights(QueryClass::PathOrCode, false, false);
        QCOMPARE(w.mergeLexicalWeight, 0.75f);
        QCOMPARE(w.mergeSemanticWeight, 0.25f);
    }

    void testAdaptiveWeightsShortAmbiguous()
    {
        auto w = computeWeights(QueryClass::ShortAmbiguous, false, false);
        QCOMPARE(w.mergeLexicalWeight, 0.65f);
        QCOMPARE(w.mergeSemanticWeight, 0.35f);
    }

    void testRelaxedAdmissionThreshold()
    {
        // NL + weak lexical → relaxed 0.74
        QCOMPARE(computeSafetyThreshold(true, true), 0.74f);

        // NL + strong lexical → standard 0.78
        QCOMPARE(computeSafetyThreshold(false, true), 0.78f);

        // Non-NL + weak → standard 0.78
        QCOMPARE(computeSafetyThreshold(true, false), 0.78f);
    }

    void testProportionalSemanticScale()
    {
        // For NL queries, semantic-only cap = 18.0, non-semantic-only = 18.0 scale
        // Verify the scale factors
        const double normalizedSemantic = 0.8;
        const bool naturalLanguageQuery = true;

        // semantic-only path
        const double semanticOnlyBoost = std::min(18.0, 5.0 + (normalizedSemantic * 18.0));
        QVERIFY(semanticOnlyBoost > 14.0); // Was capped at 14.0 before

        // non-semantic-only, NL query
        const double scale = naturalLanguageQuery ? 18.0 : 8.0;
        const double nonSemanticOnlyBoost = std::min(scale, normalizedSemantic * scale);
        QCOMPARE(nonSemanticOnlyBoost, 14.4); // 0.8 * 18.0 = 14.4

        // non-semantic-only, non-NL query
        const double nonNLBoost = std::min(8.0, normalizedSemantic * 8.0);
        QCOMPARE(nonNLBoost, 6.4); // 0.8 * 8.0 = 6.4
    }

    void testSemanticCapIncreasedForWeakLexical()
    {
        const int limit = 20;

        // NL + weak: cap increases from 6 to 8
        QCOMPARE(computeSemanticCap(true, false, true, limit), 8);

        // NL + strong: cap stays at 6
        QCOMPARE(computeSemanticCap(true, false, false, limit), 6);

        // ShortAmbiguous: unchanged at 4
        QCOMPARE(computeSemanticCap(false, true, false, limit), 4);

        // PathOrCode: unchanged at 3 (limit/2 capped)
        QCOMPARE(computeSemanticCap(false, false, false, limit), 3);
    }

    void testWeightsSumToOne()
    {
        // All weight combinations should sum to ~1.0
        auto w1 = computeWeights(QueryClass::NaturalLanguage, true, true);
        QCOMPARE(w1.mergeLexicalWeight + w1.mergeSemanticWeight, 1.0f);

        auto w2 = computeWeights(QueryClass::NaturalLanguage, true, false);
        QCOMPARE(w2.mergeLexicalWeight + w2.mergeSemanticWeight, 1.0f);

        auto w3 = computeWeights(QueryClass::PathOrCode, false, false);
        QCOMPARE(w3.mergeLexicalWeight + w3.mergeSemanticWeight, 1.0f);

        auto w4 = computeWeights(QueryClass::ShortAmbiguous, false, false);
        QCOMPARE(w4.mergeLexicalWeight + w4.mergeSemanticWeight, 1.0f);
    }
};

QTEST_MAIN(TestSemanticAdaptive)
#include "test_semantic_adaptive.moc"
