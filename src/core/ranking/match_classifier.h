#pragma once

#include "core/shared/search_result.h"
#include <QString>

namespace bs {

class MatchClassifier {
public:
    // Classify the match type between a query and a file.
    // Evaluated in priority order: ExactName, PrefixName, ContainsName,
    // ExactPath, PrefixPath, Fuzzy. Content is set by the caller (FTS5).
    static MatchType classify(const QString& query, const QString& fileName,
                              const QString& filePath);

    // Compute Levenshtein edit distance between two strings (case-insensitive).
    static int editDistance(const QString& a, const QString& b);

    // Check if the query is a fuzzy match for the file name (without extension).
    static bool isFuzzyMatch(const QString& query, const QString& fileName,
                             int maxDistance = 2);

private:
    // Strip the file extension from a filename.
    static QString stripExtension(const QString& fileName);
};

} // namespace bs
