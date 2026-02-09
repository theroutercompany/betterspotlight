#pragma once

#include "core/query/structured_query.h"

#include <QString>

#include <vector>

namespace bs {

class EntityExtractor {
public:
    static std::vector<Entity> extract(const QString& originalQuery);
};

} // namespace bs
