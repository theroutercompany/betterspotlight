#include "core/ranking/scorer.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>

namespace bs {

Scorer::Scorer(const ScoringWeights& weights)
    : m_weights(weights)
{
}

double Scorer::computeBaseMatchScore(MatchType matchType, double bm25RawScore) const
{
    switch (matchType) {
    case MatchType::ExactName:
        return static_cast<double>(m_weights.exactNameWeight);
    case MatchType::PrefixName:
        return static_cast<double>(m_weights.prefixNameWeight);
    case MatchType::ContainsName:
        return static_cast<double>(m_weights.containsNameWeight);
    case MatchType::ExactPath:
        return static_cast<double>(m_weights.exactPathWeight);
    case MatchType::PrefixPath:
        return static_cast<double>(m_weights.prefixPathWeight);
    case MatchType::Content:
        // FTS5 bm25 returns lower-is-better values (often negative). Convert
        // to a positive lexical signal so stronger matches get higher scores.
        return (std::max(0.0, -bm25RawScore) * m_weights.contentMatchWeight)
               + ((bm25RawScore < 0.0) ? 1e-12 : 0.0);
    case MatchType::Fuzzy:
        return static_cast<double>(m_weights.fuzzyMatchWeight);
    }
    return 0.0;
}

double Scorer::computeRecencyBoost(double modifiedAtEpoch) const
{
    if (m_weights.recencyWeight <= 0 || m_weights.recencyDecayDays <= 0) {
        return 0.0;
    }

    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const double timeSince = now - modifiedAtEpoch;

    if (timeSince < 0.0) {
        // File appears to be modified in the future; give full boost
        return static_cast<double>(m_weights.recencyWeight);
    }

    const double decayConstant = static_cast<double>(m_weights.recencyDecayDays) * 86400.0;
    return static_cast<double>(m_weights.recencyWeight) * std::exp(-timeSince / decayConstant);
}

double Scorer::computeFrequencyBoost(int openCount, double lastOpenEpoch) const
{
    if (openCount <= 0) {
        return 0.0;
    }

    // Tiered base boost
    double baseTierBoost = 0.0;
    if (openCount >= 21) {
        baseTierBoost = static_cast<double>(m_weights.frequencyTier3Boost);
    } else if (openCount >= 6) {
        baseTierBoost = static_cast<double>(m_weights.frequencyTier2Boost);
    } else {
        // 1-5 opens
        baseTierBoost = static_cast<double>(m_weights.frequencyTier1Boost);
    }

    // Recency modifier: boost decays if the file hasn't been opened recently
    if (lastOpenEpoch > 0.0) {
        const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
        const double daysSinceLastOpen = (now - lastOpenEpoch) / 86400.0;
        const double recencyModifier = 0.5 + 0.5 * std::exp(-daysSinceLastOpen / 30.0);
        return baseTierBoost * recencyModifier;
    }

    // No last-open date available: apply without recency modifier
    return baseTierBoost;
}

double Scorer::computeJunkPenalty(const QString& filePath) const
{
    if (m_weights.junkPenaltyWeight <= 0) {
        return 0.0;
    }

    // Important dotfiles should never be penalized.
    const int lastSlash = filePath.lastIndexOf(QLatin1Char('/'));
    const QString fileName = (lastSlash >= 0) ? filePath.mid(lastSlash + 1)
                                               : filePath;
    if (isImportantDotfile(fileName)) {
        return 0.0;
    }

    const QString pathLower = filePath.toLower();
    for (const auto& pattern : junkPatterns()) {
        if (pathLower.contains(pattern)) {
            LOG_DEBUG(bsRanking, "junkPenalty: file='%s' matched pattern='%s'",
                      qUtf8Printable(filePath), qUtf8Printable(pattern));
            return static_cast<double>(m_weights.junkPenaltyWeight);
        }
    }

    return 0.0;
}

double Scorer::computePinnedBoost(bool isPinned) const
{
    if (isPinned) {
        return static_cast<double>(m_weights.pinnedBoostWeight);
    }
    return 0.0;
}

bool Scorer::isImportantDotfile(const QString& fileName)
{
    static const std::vector<QString> importantDotfiles = {
        QStringLiteral(".gitignore"),
        QStringLiteral(".gitattributes"),
        QStringLiteral(".gitmodules"),
        QStringLiteral(".editorconfig"),
        QStringLiteral(".env"),
        QStringLiteral(".envrc"),
        QStringLiteral(".zshrc"),
        QStringLiteral(".bashrc"),
        QStringLiteral(".profile"),
        QStringLiteral(".vimrc"),
        QStringLiteral(".tmux.conf"),
        QStringLiteral(".prettierrc"),
        QStringLiteral(".eslintrc"),
        QStringLiteral(".npmrc"),
        QStringLiteral(".bsignore"),
    };

    const QString normalized = fileName.toLower();
    return std::find(importantDotfiles.begin(), importantDotfiles.end(),
                     normalized) != importantDotfiles.end();
}

const std::vector<QString>& Scorer::junkPatterns()
{
    static const std::vector<QString> patterns = {
        QStringLiteral("/node_modules/"),
        QStringLiteral("/.build/"),
        QStringLiteral("/__pycache__/"),
        QStringLiteral("/.cache/"),
        QStringLiteral("/deriveddata/"),
        QStringLiteral("/.trash/"),
        QStringLiteral("/vendor/bundle/"),
        QStringLiteral("/.git/"),
    };
    return patterns;
}

ScoreBreakdown Scorer::computeScore(const SearchResult& result,
                                     const QueryContext& context,
                                     double bm25RawScore) const
{
    ScoreBreakdown breakdown;

    // 1. Base match score
    breakdown.baseMatchScore = computeBaseMatchScore(result.matchType, bm25RawScore);

    if (result.matchType == MatchType::Fuzzy && result.fuzzyDistance > 1) {
        const double penalty = (result.fuzzyDistance == 2) ? 0.5 : 0.25;
        breakdown.baseMatchScore *= penalty;
    }

    // 2. Recency boost (parse modificationDate as epoch seconds)
    if (!result.modificationDate.isEmpty()) {
        bool ok = false;
        const double epoch = result.modificationDate.toDouble(&ok);
        if (ok) {
            breakdown.recencyBoost = computeRecencyBoost(epoch);
        } else {
            // Try parsing as ISO 8601
            const QDateTime dt = QDateTime::fromString(result.modificationDate, Qt::ISODate);
            if (dt.isValid()) {
                breakdown.recencyBoost = computeRecencyBoost(
                    static_cast<double>(dt.toSecsSinceEpoch()));
            }
        }
    }

    // 3. Frequency boost
    double lastOpenEpoch = 0.0;
    if (!result.lastOpenDate.isEmpty()) {
        bool ok = false;
        lastOpenEpoch = result.lastOpenDate.toDouble(&ok);
        if (!ok) {
            const QDateTime dt = QDateTime::fromString(result.lastOpenDate, Qt::ISODate);
            if (dt.isValid()) {
                lastOpenEpoch = static_cast<double>(dt.toSecsSinceEpoch());
            }
        }
    }
    breakdown.frequencyBoost = computeFrequencyBoost(result.openCount, lastOpenEpoch);

    // Content-only matches can otherwise be dominated by recency/frequency and
    // surface broad "chat notes" over directly matching filenames. Dampen these
    // boosts for content matches while preserving lexical BM25 strength.
    if (result.matchType == MatchType::Content) {
        breakdown.recencyBoost *= 0.25;
        breakdown.frequencyBoost *= 0.5;
    }

    // 4. Context boost (CWD proximity + app context)
    double ctxBoost = 0.0;
    if (context.cwdPath.has_value() && !context.cwdPath->isEmpty()) {
        ctxBoost += m_contextSignals.cwdProximityBoost(
            result.path, *context.cwdPath, m_weights.cwdBoostWeight);
    }
    if (context.frontmostAppBundleId.has_value() && !context.frontmostAppBundleId->isEmpty()) {
        ctxBoost += m_contextSignals.appContextBoost(
            result.path, *context.frontmostAppBundleId, m_weights.appContextBoostWeight);
    }
    breakdown.contextBoost = ctxBoost;

    // 5. Pinned boost
    breakdown.pinnedBoost = computePinnedBoost(result.isPinned);

    // 6. Junk penalty
    breakdown.junkPenalty = computeJunkPenalty(result.path);

    // semanticBoost is M2 — leave at 0.0

    LOG_DEBUG(bsRanking,
              "computeScore: id=%lld base=%.1f recency=%.1f freq=%.1f ctx=%.1f "
              "pinned=%.1f junk=%.1f",
              result.itemId, breakdown.baseMatchScore, breakdown.recencyBoost,
              breakdown.frequencyBoost, breakdown.contextBoost,
              breakdown.pinnedBoost, breakdown.junkPenalty);

    return breakdown;
}

void Scorer::rankResults(std::vector<SearchResult>& results,
                          const QueryContext& context) const
{
    // Compute score for each result
    for (auto& result : results) {
        const ScoreBreakdown breakdown = computeScore(result, context,
                                                      result.bm25RawScore);
        result.scoreBreakdown = breakdown;

        // Final score: max(0, base + recency + frequency + context + pinned + semantic
        //              + crossEncoder + structuredQuery - junk)
        const double finalScore = breakdown.baseMatchScore
                                  + breakdown.recencyBoost
                                  + breakdown.frequencyBoost
                                  + breakdown.contextBoost
                                  + breakdown.pinnedBoost
                                  + breakdown.semanticBoost
                                  + breakdown.crossEncoderBoost
                                  + breakdown.structuredQueryBoost
                                  - breakdown.junkPenalty;
        result.score = std::max(0.0, finalScore);
    }

    // Sort by (finalScore DESC, itemId ASC) for stable tie-breaking
    std::stable_sort(results.begin(), results.end(),
                     [](const SearchResult& a, const SearchResult& b) {
                         if (a.score != b.score) {
                             return a.score > b.score; // Descending
                         }
                         return a.itemId < b.itemId; // Ascending (tie-break)
                     });

    LOG_INFO(bsRanking, "rankResults: ranked %zu results", results.size());
}

} // namespace bs
