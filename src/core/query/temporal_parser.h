#pragma once

#include "core/query/structured_query.h"

#include <QString>

#include <optional>

namespace bs {

class TemporalParser {
public:
    static std::optional<TemporalRange> parse(const QString& query);
};

} // namespace bs
