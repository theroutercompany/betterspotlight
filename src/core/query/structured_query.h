#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace bs {

enum class EntityType { Person, Place, Organization, Other };

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
    float nluConfidence = 0.0f;
};

} // namespace bs
