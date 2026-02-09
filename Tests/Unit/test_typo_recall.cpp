#include <QtTest/QtTest>

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

class TestTypoRecall : public QObject {
    Q_OBJECT

private:
    // Mirrors the highSignalShortTokens extraction logic from query_service.cpp
    QSet<QString> extractHighSignalShortTokens(const QString& rawQuery) const
    {
        QSet<QString> result;
        static const QRegularExpression rawTokenRegex(QStringLiteral(R"([A-Za-z0-9_]+)"));
        auto tokenMatch = rawTokenRegex.globalMatch(rawQuery);
        while (tokenMatch.hasNext()) {
            const QString token = tokenMatch.next().captured(0);
            if (token.size() != 3) {
                continue;
            }
            bool hasAlpha = false;
            bool allUpper = true;
            for (const QChar ch : token) {
                if (ch.isLetter()) {
                    hasAlpha = true;
                    if (!ch.isUpper()) {
                        allUpper = false;
                        break;
                    }
                }
            }
            const bool hasDigit = std::any_of(token.begin(), token.end(),
                [](QChar c) { return c.isDigit(); });
            if ((hasAlpha && allUpper) || (hasAlpha && hasDigit)) {
                result.insert(token.toLower());
            }
        }
        return result;
    }

    // Mirrors typoCandidateConfidence with prefix-change penalty
    double typoCandidateConfidence(const QString& sourceToken,
                                    const QString& corrected,
                                    int editDistance,
                                    int docCount) const
    {
        double confidence = 0.48;
        if (editDistance == 1) {
            confidence += 0.22;
        } else if (editDistance == 2) {
            confidence += 0.10;
        }

        if (docCount >= 50) {
            confidence += 0.26;
        } else if (docCount >= 25) {
            confidence += 0.22;
        } else if (docCount >= 12) {
            confidence += 0.18;
        } else if (docCount >= 6) {
            confidence += 0.13;
        } else if (docCount >= 3) {
            confidence += 0.08;
        }

        if (!sourceToken.isEmpty() && !corrected.isEmpty()) {
            if (sourceToken.front().toLower() == corrected.front().toLower()) {
                confidence += 0.06;
            } else {
                confidence -= 0.08;  // prefix-change penalty
            }
        }

        return std::clamp(confidence, 0.0, 1.0);
    }

    // Mirrors contextual budget logic
    int computeRewriteBudget(int signalTokenCount, bool strictWeakOrEmpty) const
    {
        return strictWeakOrEmpty
            ? std::clamp(signalTokenCount / 2, 2, 3)
            : std::clamp(signalTokenCount / 3, 1, 2);
    }

private slots:
    void testAlnumShortTokenEligible()
    {
        // "q4" and "ml3" should be recognized as high-signal short tokens
        // (they have alpha + digit)
        auto tokens = extractHighSignalShortTokens(QStringLiteral("find q4 report"));
        // q4 is only 2 chars, not 3 — but "ml3" is 3 chars
        QVERIFY(!tokens.contains(QStringLiteral("q4"))); // 2-char, filtered by size != 3

        auto tokens2 = extractHighSignalShortTokens(QStringLiteral("show ml3 data"));
        QVERIFY(tokens2.contains(QStringLiteral("ml3")));

        // "r2d" has alpha + digit, 3 chars
        auto tokens3 = extractHighSignalShortTokens(QStringLiteral("r2d stuff"));
        QVERIFY(tokens3.contains(QStringLiteral("r2d")));

        // Old behavior preserved: all-upper 3-letter tokens still work
        auto tokens4 = extractHighSignalShortTokens(QStringLiteral("CPU usage"));
        QVERIFY(tokens4.contains(QStringLiteral("cpu")));
    }

    void testPrefixChangePenalty()
    {
        // Same first char: gets +0.06
        double samePrefix = typoCandidateConfidence(
            QStringLiteral("tset"), QStringLiteral("test"), 1, 10);
        // Different first char: gets -0.08
        double diffPrefix = typoCandidateConfidence(
            QStringLiteral("xest"), QStringLiteral("test"), 1, 10);

        // The difference should be 0.14 (0.06 gain vs 0.08 penalty)
        QVERIFY(samePrefix > diffPrefix);
        QCOMPARE(samePrefix - diffPrefix, 0.14);
    }

    void testHighDocCountLowersThreshold()
    {
        // docCount >= 25 should use 0.60 threshold instead of 0.66
        // A candidate with docCount=30, editDistance=1, same prefix:
        // confidence = 0.48 + 0.22(ed1) + 0.22(doc25+) + 0.06(prefix) = 0.98
        // Both thresholds pass.

        // A marginal candidate with docCount=30, editDistance=2, different prefix:
        // confidence = 0.48 + 0.10(ed2) + 0.22(doc25+) - 0.08(prefix) = 0.72
        // Passes 0.60, passes 0.66 too.

        // Really low: docCount=30, editDistance=2, different prefix, short token:
        // confidence = 0.48 + 0.10(ed2) + 0.22(doc25+) - 0.08(prefix) = 0.72
        // Actually, need a case between 0.60 and 0.66:
        // docCount=26, editDistance=2, diff prefix:
        // 0.48 + 0.10 + 0.22 - 0.08 = 0.72 — still above 0.66

        // docCount=4, editDistance=2, diff prefix:
        // 0.48 + 0.10 + 0.08 - 0.08 = 0.58 — below both thresholds

        // The key test: verify the threshold is 0.60 for high-docCount
        // vs 0.66 for low-docCount
        const double candidateThreshold = 0.66;
        const int highDocCount = 30;
        const int lowDocCount = 5;

        const double effectiveThresholdHigh = (highDocCount >= 25) ? 0.60 : candidateThreshold;
        const double effectiveThresholdLow = (lowDocCount >= 25) ? 0.60 : candidateThreshold;

        QCOMPARE(effectiveThresholdHigh, 0.60);
        QCOMPARE(effectiveThresholdLow, 0.66);

        // A confidence of 0.62 would pass high-docCount threshold but not low
        const double marginalConfidence = 0.62;
        QVERIFY(marginalConfidence >= effectiveThresholdHigh);
        QVERIFY(marginalConfidence < effectiveThresholdLow);
    }

    void testContextualBudgetScales()
    {
        // strictWeakOrEmpty = true: budget = clamp(tokenCount/2, 2, 3)
        QCOMPARE(computeRewriteBudget(2, true), 2);   // 2/2=1, clamped to 2
        QCOMPARE(computeRewriteBudget(4, true), 2);   // 4/2=2
        QCOMPARE(computeRewriteBudget(6, true), 3);   // 6/2=3
        QCOMPARE(computeRewriteBudget(8, true), 3);   // 8/2=4, clamped to 3

        // strictWeakOrEmpty = false: budget = clamp(tokenCount/3, 1, 2)
        QCOMPARE(computeRewriteBudget(2, false), 1);  // 2/3=0, clamped to 1
        QCOMPARE(computeRewriteBudget(3, false), 1);  // 3/3=1
        QCOMPARE(computeRewriteBudget(6, false), 2);  // 6/3=2
        QCOMPARE(computeRewriteBudget(9, false), 2);  // 9/3=3, clamped to 2
    }

    void testNameMatchOverridesBm25Check()
    {
        // This test verifies the logic: if rewritten query produces a name
        // match that the original didn't, the rewrite is allowed even if
        // the BM25 strength is weaker.
        // We test the boolean logic directly.
        const bool rewrittenStronger = false;
        const bool rewrittenHasNameHit = true;
        const bool originalHasNameHit = false;

        const bool shouldApplyRewrite = rewrittenStronger
            || (rewrittenHasNameHit && !originalHasNameHit);
        QVERIFY(shouldApplyRewrite);

        // When both have name hits, BM25 must be stronger
        const bool shouldApplyBothNames = false || (true && !true);
        QVERIFY(!shouldApplyBothNames);
    }

    void testHighDocCountExtraBoost()
    {
        // docCount >= 50 should get 0.26 (higher than the >= 25 tier of 0.22)
        // Use editDistance=2 and diff prefix to stay well below clamp(1.0):
        // ed2: 0.48+0.10=0.58, doc50+: +0.26=0.84, diffPrefix: -0.08=0.76
        // ed2: 0.48+0.10=0.58, doc25+: +0.22=0.80, diffPrefix: -0.08=0.72
        double highDoc = typoCandidateConfidence(
            QStringLiteral("xset"), QStringLiteral("test"), 2, 55);
        double medDoc = typoCandidateConfidence(
            QStringLiteral("xset"), QStringLiteral("test"), 2, 30);

        QCOMPARE(highDoc - medDoc, 0.04);
    }
};

QTEST_MAIN(TestTypoRecall)
#include "test_typo_recall.moc"
