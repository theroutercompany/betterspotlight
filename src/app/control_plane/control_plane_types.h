#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace bs {

enum class AppLifecyclePhase {
    Starting,
    Running,
    ShuttingDown,
    Stopped,
};

enum class ManagedServiceState {
    Registered,
    Starting,
    Ready,
    Degraded,
    Backoff,
    Crashed,
    Stopped,
    GivingUp,
};

struct ServiceRuntimeState {
    QString name;
    ManagedServiceState state = ManagedServiceState::Registered;
    bool running = false;
    bool ready = false;
    qint64 crashCount = 0;
    qint64 pid = 0;
    qint64 updatedAtMs = 0;
    QString reason;
};

struct HealthComponentV2 {
    QString state = QStringLiteral("unavailable");
    QString reason;
    qint64 lastUpdatedMs = 0;
    qint64 stalenessMs = 0;
    QJsonObject metrics;
};

inline QString appLifecyclePhaseToString(AppLifecyclePhase phase)
{
    switch (phase) {
    case AppLifecyclePhase::Starting:
        return QStringLiteral("starting");
    case AppLifecyclePhase::Running:
        return QStringLiteral("running");
    case AppLifecyclePhase::ShuttingDown:
        return QStringLiteral("shutting_down");
    case AppLifecyclePhase::Stopped:
        return QStringLiteral("stopped");
    }
    return QStringLiteral("unknown");
}

inline QString managedServiceStateToString(ManagedServiceState state)
{
    switch (state) {
    case ManagedServiceState::Registered:
        return QStringLiteral("registered");
    case ManagedServiceState::Starting:
        return QStringLiteral("starting");
    case ManagedServiceState::Ready:
        return QStringLiteral("ready");
    case ManagedServiceState::Degraded:
        return QStringLiteral("degraded");
    case ManagedServiceState::Backoff:
        return QStringLiteral("backoff");
    case ManagedServiceState::Crashed:
        return QStringLiteral("crashed");
    case ManagedServiceState::Stopped:
        return QStringLiteral("stopped");
    case ManagedServiceState::GivingUp:
        return QStringLiteral("giving_up");
    }
    return QStringLiteral("unknown");
}

inline ManagedServiceState managedServiceStateFromString(const QString& state)
{
    const QString normalized = state.trimmed().toLower();
    if (normalized == QLatin1String("starting")) {
        return ManagedServiceState::Starting;
    }
    if (normalized == QLatin1String("ready") || normalized == QLatin1String("running")) {
        return ManagedServiceState::Ready;
    }
    if (normalized == QLatin1String("degraded")) {
        return ManagedServiceState::Degraded;
    }
    if (normalized == QLatin1String("backoff")) {
        return ManagedServiceState::Backoff;
    }
    if (normalized == QLatin1String("crashed")) {
        return ManagedServiceState::Crashed;
    }
    if (normalized == QLatin1String("stopped")) {
        return ManagedServiceState::Stopped;
    }
    if (normalized == QLatin1String("giving_up")) {
        return ManagedServiceState::GivingUp;
    }
    return ManagedServiceState::Registered;
}

inline QJsonObject serviceRuntimeStateToJson(const ServiceRuntimeState& service)
{
    QJsonObject out;
    out[QStringLiteral("name")] = service.name;
    out[QStringLiteral("state")] = managedServiceStateToString(service.state);
    out[QStringLiteral("running")] = service.running;
    out[QStringLiteral("ready")] = service.ready;
    out[QStringLiteral("crashCount")] = service.crashCount;
    out[QStringLiteral("pid")] = service.pid;
    out[QStringLiteral("updatedAtMs")] = service.updatedAtMs;
    out[QStringLiteral("reason")] = service.reason;
    return out;
}

inline ServiceRuntimeState serviceRuntimeStateFromJson(const QJsonObject& json)
{
    ServiceRuntimeState out;
    out.name = json.value(QStringLiteral("name")).toString();
    out.state = managedServiceStateFromString(json.value(QStringLiteral("state")).toString());
    out.running = json.value(QStringLiteral("running")).toBool(false);
    out.ready = json.value(QStringLiteral("ready")).toBool(false);
    out.crashCount = json.value(QStringLiteral("crashCount")).toInteger();
    out.pid = json.value(QStringLiteral("pid")).toInteger();
    out.updatedAtMs = json.value(QStringLiteral("updatedAtMs")).toInteger();
    out.reason = json.value(QStringLiteral("reason")).toString();
    return out;
}

inline QJsonObject healthComponentToJson(const HealthComponentV2& component)
{
    QJsonObject out;
    out[QStringLiteral("state")] = component.state;
    out[QStringLiteral("reason")] = component.reason;
    out[QStringLiteral("lastUpdatedMs")] = component.lastUpdatedMs;
    out[QStringLiteral("stalenessMs")] = component.stalenessMs;
    out[QStringLiteral("metrics")] = component.metrics;
    return out;
}

} // namespace bs
