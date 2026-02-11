#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace bs {

enum class EntityType { Person, Place, Organization, Other };
enum class QueryClass {
    NaturalLanguage,
    PathOrCode,
    ShortAmbiguous,
    Unknown,
};

enum class QueryDomain {
    PersonalDocs,
    DevCode,
    Finance,
    Media,
    General,
    Unknown,
};

struct Entity {
    QString text;
    EntityType type;
};

struct TemporalRange {
    double startEpoch = 0.0;
    double endEpoch = 0.0;
};

struct StructuredQuery {
    QString originalQuery;
    QString cleanedQuery;
    std::vector<Entity> entities;
    std::optional<TemporalRange> temporal;
    std::optional<QString> docTypeIntent;
    std::vector<QString> locationHints;
    std::vector<QString> keyTokens;
    QueryClass queryClass = QueryClass::Unknown;
    float queryClassConfidence = 0.0f;
    QueryDomain queryDomain = QueryDomain::Unknown;
    float queryDomainConfidence = 0.0f;
    float semanticNeedScore = 0.0f;
    // Compatibility alias for callers expecting the legacy field.
    float nluConfidence = 0.0f;
};

} // namespace bs
