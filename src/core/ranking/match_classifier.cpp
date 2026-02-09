#include "core/ranking/match_classifier.h"
#include "core/shared/logging.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <vector>

namespace bs {

namespace {

// Regex: \s*[-–—_]+\s* → collapse dash/en-dash/em-dash/underscore separators to single space
QString normalizeSeparators(const QString& s)
{
    static const QRegularExpression dashSep(
        QStringLiteral("\\s*[-\\x{2013}\\x{2014}_]+\\s*"));
    static const QRegularExpression multiSpace(QStringLiteral(R"(\s{2,})"));
    QString result = s;
    result.replace(dashSep, QStringLiteral(" "));
    result.replace(multiSpace, QStringLiteral(" "));
    return result.trimmed();
}

} // namespace

QString MatchClassifier::stripExtension(const QString& fileName)
{
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        // No extension, or dotfile like ".bashrc" — return as-is
        return fileName;
    }
    return fileName.left(dot);
}

MatchType MatchClassifier::classify(const QString& query, const QString& fileName,
                                    const QString& filePath)
{
    if (query.isEmpty()) {
        return MatchType::Content;
    }

    const QString queryLower = query.toLower();
    const QString nameLower = fileName.toLower();
    const QString nameNoExtLower = stripExtension(fileName).toLower();

    const QString queryNorm = normalizeSeparators(queryLower);
    const QString nameNoExtNorm = normalizeSeparators(nameNoExtLower);

    // 1. ExactName: fileName (without extension) matches query, case-insensitive
    if (nameNoExtNorm == queryNorm) {
        LOG_DEBUG(bsRanking, "classify: ExactName match for query='%s' file='%s'",
                  qUtf8Printable(query), qUtf8Printable(fileName));
        return MatchType::ExactName;
    }

    // 2. PrefixName: fileName starts with query, case-insensitive
    if (nameLower.startsWith(queryLower)) {
        LOG_DEBUG(bsRanking, "classify: PrefixName match for query='%s' file='%s'",
                  qUtf8Printable(query), qUtf8Printable(fileName));
        return MatchType::PrefixName;
    }

    // 3. ContainsName: fileName contains query, case-insensitive
    if (nameLower.contains(queryLower)) {
        LOG_DEBUG(bsRanking, "classify: ContainsName match for query='%s' file='%s'",
                  qUtf8Printable(query), qUtf8Printable(fileName));
        return MatchType::ContainsName;
    }

    // 4. ExactPath: full path matches query exactly
    if (filePath == query) {
        LOG_DEBUG(bsRanking, "classify: ExactPath match for query='%s'",
                  qUtf8Printable(query));
        return MatchType::ExactPath;
    }

    // 5. PrefixPath: full path starts with query
    if (filePath.startsWith(query)) {
        LOG_DEBUG(bsRanking, "classify: PrefixPath match for query='%s'",
                  qUtf8Printable(query));
        return MatchType::PrefixPath;
    }

    // 6. Content is set by caller (FTS5 results), not determined here.

    // 7. Fuzzy: edit distance within threshold
    if (isFuzzyMatch(query, fileName)) {
        LOG_DEBUG(bsRanking, "classify: Fuzzy match for query='%s' file='%s'",
                  qUtf8Printable(query), qUtf8Printable(fileName));
        return MatchType::Fuzzy;
    }

    // Default: Content (caller sets this for FTS5 results)
    return MatchType::Content;
}

int MatchClassifier::editDistance(const QString& a, const QString& b)
{
    const QString aLower = a.toLower();
    const QString bLower = b.toLower();
    const int m = aLower.length();
    const int n = bLower.length();

    // Use two-row DP to save memory
    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);

    // Base case: distance from empty string
    for (int j = 0; j <= n; ++j) {
        prev[j] = j;
    }

    for (int i = 1; i <= m; ++i) {
        curr[0] = i;
        for (int j = 1; j <= n; ++j) {
            if (aLower.at(i - 1) == bLower.at(j - 1)) {
                curr[j] = prev[j - 1];
            } else {
                curr[j] = 1 + std::min({prev[j],       // deletion
                                         curr[j - 1],   // insertion
                                         prev[j - 1]}); // substitution
            }
        }
        std::swap(prev, curr);
    }

    return prev[n];
}

bool MatchClassifier::isFuzzyMatch(const QString& query, const QString& fileName,
                                   int maxDistance)
{
    if (query.isEmpty() || fileName.isEmpty()) {
        return false;
    }

    const QString nameNoExt = stripExtension(fileName);
    const int dist = editDistance(query, nameNoExt);
    return dist <= maxDistance;
}

} // namespace bs
