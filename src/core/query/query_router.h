#pragma once

#include "core/query/structured_query.h"

#include <QString>

#include <vector>

namespace bs {

struct QueryRouterResult {
    QueryClass queryClass = QueryClass::Unknown;
    QueryDomain queryDomain = QueryDomain::Unknown;
    float routerConfidence = 0.0f;
    float queryDomainConfidence = 0.0f;
    float semanticNeedScore = 0.0f;
    bool valid = false;
};

QString queryClassToString(QueryClass value);
QString queryDomainToString(QueryDomain value);

class QueryRouter {
public:
    static QueryRouterResult route(const QString& originalQuery,
                                   const QString& cleanedQuery,
                                   const std::vector<QString>& keyTokens);
};

} // namespace bs
