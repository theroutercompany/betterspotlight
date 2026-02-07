#pragma once

#include <QString>
#include <QJsonObject>
#include <optional>
#include <vector>

namespace bs {

// Match types with base point values (doc 06)
enum class MatchType {
    ExactName,      // 200 points
    PrefixName,     // 150 points
    ContainsName,   // 100 points
    ExactPath,      // 90 points
    PrefixPath,     // 80 points
    Content,        // Variable (BM25 * contentMatchWeight)
    Fuzzy,          // 30 points
};

int matchTypeBasePoints(MatchType type);
QString matchTypeToString(MatchType type);

struct Highlight {
    int offset = 0;
    int length = 0;
};

// Context signals provided with each query (doc 06)
struct QueryContext {
    std::optional<QString> cwdPath;
    std::optional<QString> frontmostAppBundleId;
    std::vector<QString> recentPaths;
};

// Score breakdown for debugging/transparency (doc 06)
struct ScoreBreakdown {
    double baseMatchScore = 0.0;
    double recencyBoost = 0.0;
    double frequencyBoost = 0.0;
    double contextBoost = 0.0;
    double pinnedBoost = 0.0;
    double junkPenalty = 0.0;
    double semanticBoost = 0.0;
};

struct SearchResult {
    int64_t itemId = 0;
    QString path;
    QString name;
    QString kind;
    MatchType matchType = MatchType::Content;
    double score = 0.0;
    QString snippet;
    std::vector<Highlight> highlights;
    int64_t fileSize = 0;
    QString modificationDate;
    bool isPinned = false;
    int openCount = 0;
    QString lastOpenDate;
    ScoreBreakdown scoreBreakdown;
};

} // namespace bs
