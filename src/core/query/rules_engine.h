#pragma once

#include "core/query/structured_query.h"

#include <QString>

namespace bs {

class RulesEngine {
public:
    static StructuredQuery analyze(const QString& originalQuery);
};

} // namespace bs
