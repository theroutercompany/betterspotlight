#include "core/shared/search_result.h"

namespace bs {

int matchTypeBasePoints(MatchType type)
{
    switch (type) {
    case MatchType::ExactName:    return 200;
    case MatchType::PrefixName:   return 150;
    case MatchType::ContainsName: return 100;
    case MatchType::ExactPath:    return 90;
    case MatchType::PrefixPath:   return 80;
    case MatchType::Content:      return 50;  // Typical BM25 range; actual varies
    case MatchType::Fuzzy:        return 30;
    }
    return 0;
}

QString matchTypeToString(MatchType type)
{
    switch (type) {
    case MatchType::ExactName:    return QStringLiteral("exactNameMatch");
    case MatchType::PrefixName:   return QStringLiteral("prefixNameMatch");
    case MatchType::ContainsName: return QStringLiteral("containsNameMatch");
    case MatchType::ExactPath:    return QStringLiteral("exactPathMatch");
    case MatchType::PrefixPath:   return QStringLiteral("prefixPathMatch");
    case MatchType::Content:      return QStringLiteral("contentMatch");
    case MatchType::Fuzzy:        return QStringLiteral("fuzzyMatch");
    }
    return QStringLiteral("unknown");
}

} // namespace bs
