#pragma once

#include <QString>

namespace bs {

struct NormalizedQuery {
    QString original;
    QString normalized;
};

class QueryNormalizer {
public:
    static NormalizedQuery normalize(const QString& raw);
};

} // namespace bs
