#pragma once

#include <QString>

namespace bs::runtime_mode_policy {

inline bool allowUnsupportedRuntimeModes()
{
    const QString value =
        qEnvironmentVariable("BETTERSPOTLIGHT_ALLOW_UNSUPPORTED_RUNTIME_MODES")
            .trimmed()
            .toLower();
    if (value.isEmpty()) {
        return false;
    }
    return value == QLatin1String("1")
        || value == QLatin1String("true")
        || value == QLatin1String("yes")
        || value == QLatin1String("on");
}

inline bool isUnsupportedProductionPipelineMode(const QString& mode)
{
    return mode == QLatin1String("legacy") || mode == QLatin1String("dual");
}

inline bool isUnsupportedInferenceSupervisorMode(const QString& mode)
{
    return mode == QLatin1String("legacy") || mode == QLatin1String("dual");
}

inline QString effectivePipelineActorMode(const QString& requestedMode,
                                          bool* coercedOut = nullptr)
{
    const QString normalized = requestedMode.trimmed().toLower();
    const bool coerced =
        !allowUnsupportedRuntimeModes() && isUnsupportedProductionPipelineMode(normalized);
    if (coercedOut) {
        *coercedOut = coerced;
    }
    if (coerced) {
        return QStringLiteral("actor_primary");
    }
    if (normalized == QLatin1String("legacy")
        || normalized == QLatin1String("dual")
        || normalized == QLatin1String("actor_primary")) {
        return normalized;
    }
    return QStringLiteral("actor_primary");
}

inline QString effectiveHealthSourceMode(const QString& requestedMode,
                                         bool* coercedOut = nullptr)
{
    const QString normalized = requestedMode.trimmed().toLower();
    const bool coerced =
        !allowUnsupportedRuntimeModes() && normalized == QLatin1String("legacy");
    if (coercedOut) {
        *coercedOut = coerced;
    }
    if (coerced) {
        return QStringLiteral("aggregator_primary");
    }
    if (normalized.isEmpty()) {
        return QStringLiteral("aggregator_primary");
    }
    return normalized;
}

inline QString effectiveControlPlaneMode(const QString& requestedMode,
                                         bool* coercedOut = nullptr)
{
    const QString normalized = requestedMode.trimmed().toLower();
    const bool coerced =
        !allowUnsupportedRuntimeModes() && normalized == QLatin1String("legacy");
    if (coercedOut) {
        *coercedOut = coerced;
    }
    if (coerced || normalized.isEmpty()) {
        return QStringLiteral("actor_primary");
    }
    return normalized;
}

inline QString effectiveInferenceSupervisorMode(const QString& requestedMode,
                                                bool* coercedOut = nullptr)
{
    const QString normalized = requestedMode.trimmed().toLower();
    const bool coerced =
        !allowUnsupportedRuntimeModes() && isUnsupportedInferenceSupervisorMode(normalized);
    if (coercedOut) {
        *coercedOut = coerced;
    }
    if (coerced) {
        return QStringLiteral("actor_primary");
    }
    if (normalized == QLatin1String("legacy")
        || normalized == QLatin1String("dual")
        || normalized == QLatin1String("actor_primary")) {
        return normalized;
    }
    return QStringLiteral("actor_primary");
}

} // namespace bs::runtime_mode_policy
