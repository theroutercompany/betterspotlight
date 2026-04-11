#pragma once

#include <QString>
#include <QStringList>

namespace bs::health_contract {

inline const QStringList& requiredServiceNames()
{
    static const QStringList names = {
        QStringLiteral("indexer"),
        QStringLiteral("query"),
        QStringLiteral("inference"),
        QStringLiteral("extractor"),
    };
    return names;
}

inline const QStringList& coreRequiredInferenceRoles()
{
    static const QStringList roles = {
        QStringLiteral("bi-encoder"),
        QStringLiteral("cross-encoder"),
    };
    return roles;
}

inline bool isRequiredService(const QString& serviceName)
{
    return requiredServiceNames().contains(serviceName);
}

inline bool isCoreRequiredInferenceRole(const QString& roleName)
{
    return coreRequiredInferenceRoles().contains(roleName);
}

inline bool isOperationalComponentState(const QString& state)
{
    return state == QLatin1String("ready") || state == QLatin1String("running");
}

} // namespace bs::health_contract
