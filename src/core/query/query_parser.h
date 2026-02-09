#pragma once

#include "core/shared/search_options.h"

#include <QString>
#include <QStringList>

namespace bs {

struct ParsedQuery {
    QString cleanedQuery;
    SearchOptions filters;
    QStringList extractedTypes;
    bool hasTypeHint = false;
};

class QueryParser {
public:
    static ParsedQuery parse(const QString& normalizedQuery);
};

} // namespace bs
