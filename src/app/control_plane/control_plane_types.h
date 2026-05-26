#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>

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

inline bool tryManagedServiceStateFromString(const QString& state, ManagedServiceState* out)
{
    const QString normalized = state.trimmed().toLower();
    if (normalized == QLatin1String("registered")) {
        if (out) *out = ManagedServiceState::Registered;
        return true;
    }
    if (normalized == QLatin1String("starting")) {
        if (out) *out = ManagedServiceState::Starting;
        return true;
    }
    if (normalized == QLatin1String("ready") || normalized == QLatin1String("running")) {
        if (out) *out = ManagedServiceState::Ready;
        return true;
    }
    if (normalized == QLatin1String("degraded")) {
        if (out) *out = ManagedServiceState::Degraded;
        return true;
    }
    if (normalized == QLatin1String("backoff")) {
        if (out) *out = ManagedServiceState::Backoff;
        return true;
    }
    if (normalized == QLatin1String("crashed")) {
        if (out) *out = ManagedServiceState::Crashed;
        return true;
    }
    if (normalized == QLatin1String("stopped")) {
        if (out) *out = ManagedServiceState::Stopped;
        return true;
    }
    if (normalized == QLatin1String("giving_up")) {
        if (out) *out = ManagedServiceState::GivingUp;
        return true;
    }
    return false;
}

inline ManagedServiceState managedServiceStateFromString(const QString& state)
{
    ManagedServiceState parsed = ManagedServiceState::Registered;
    if (tryManagedServiceStateFromString(state, &parsed)) {
        return parsed;
    }
    return ManagedServiceState::Registered;
}

inline bool managedServiceStateImpliesRunning(ManagedServiceState state)
{
    switch (state) {
    case ManagedServiceState::Starting:
    case ManagedServiceState::Ready:
    case ManagedServiceState::Degraded:
        return true;
    case ManagedServiceState::Registered:
    case ManagedServiceState::Backoff:
    case ManagedServiceState::Crashed:
    case ManagedServiceState::Stopped:
    case ManagedServiceState::GivingUp:
        return false;
    }
    return false;
}

inline bool managedServiceStateImpliesReady(ManagedServiceState state)
{
    return state == ManagedServiceState::Ready;
}

inline qint64 nonNegative(qint64 value)
{
    return std::max<qint64>(0, value);
}

inline bool jsonFieldPresent(const QJsonValue& value)
{
    return !value.isUndefined() && !value.isNull();
}

inline ManagedServiceState managedServiceStateFromRuntimeFlags(bool running, bool ready)
{
    if (ready) {
        return ManagedServiceState::Ready;
    }
    if (running) {
        return ManagedServiceState::Starting;
    }
    return ManagedServiceState::Registered;
}

inline QJsonObject serviceRuntimeStateToJson(const ServiceRuntimeState& service)
{
    QJsonObject out;
    out[QStringLiteral("name")] = service.name.trimmed();
    out[QStringLiteral("state")] = managedServiceStateToString(service.state);
    out[QStringLiteral("running")] = service.running;
    out[QStringLiteral("ready")] = service.ready;
    out[QStringLiteral("crashCount")] = nonNegative(service.crashCount);
    out[QStringLiteral("pid")] = nonNegative(service.pid);
    out[QStringLiteral("updatedAtMs")] = nonNegative(service.updatedAtMs);
    out[QStringLiteral("reason")] = service.reason.trimmed();
    return out;
}

inline ServiceRuntimeState serviceRuntimeStateFromJson(const QJsonObject& json)
{
    ServiceRuntimeState out;
    out.name = json.value(QStringLiteral("name")).toString().trimmed();
    const QJsonValue stateValue = json.value(QStringLiteral("state"));
    const QJsonValue runningValue = json.value(QStringLiteral("running"));
    const QJsonValue readyValue = json.value(QStringLiteral("ready"));
    const bool explicitRunning = runningValue.isBool();
    const bool explicitReady = readyValue.isBool();
    const bool running = explicitRunning ? runningValue.toBool(false) : false;
    const bool ready = explicitReady ? readyValue.toBool(false) : false;

    const bool explicitState = jsonFieldPresent(stateValue)
        && (!stateValue.isString() || !stateValue.toString().trimmed().isEmpty());
    bool stateParsed = false;
    if (stateValue.isString()) {
        stateParsed = tryManagedServiceStateFromString(stateValue.toString(), &out.state);
    }

    const bool invalidExplicitState = explicitState && !stateParsed;

    if (invalidExplicitState) {
        out.state = ManagedServiceState::Degraded;
        out.running = false;
        out.ready = false;
        out.reason = json.value(QStringLiteral("reason")).toString().trimmed();
        if (out.reason.isEmpty()) {
            out.reason = QStringLiteral("invalid_service_state");
        }
    } else if (!explicitState) {
        out.state = managedServiceStateFromRuntimeFlags(running, ready);
        out.running = explicitRunning ? running : managedServiceStateImpliesRunning(out.state);
        out.ready = explicitReady ? ready : managedServiceStateImpliesReady(out.state);
    } else {
        out.running = explicitRunning ? running : managedServiceStateImpliesRunning(out.state);
        out.ready = explicitReady ? ready : managedServiceStateImpliesReady(out.state);
    }

    if (out.ready && !out.running) {
        if (explicitRunning) {
            out.ready = false;
        } else {
            out.running = true;
        }
    }
    if (out.state == ManagedServiceState::Ready && !out.ready) {
        out.state = out.running ? ManagedServiceState::Starting : ManagedServiceState::Stopped;
    }
    if (!invalidExplicitState) {
        switch (out.state) {
        case ManagedServiceState::Ready:
            out.running = true;
            out.ready = true;
            break;
        case ManagedServiceState::Starting:
        case ManagedServiceState::Degraded:
            out.running = true;
            out.ready = false;
            break;
        case ManagedServiceState::Registered:
        case ManagedServiceState::Backoff:
        case ManagedServiceState::Crashed:
        case ManagedServiceState::Stopped:
        case ManagedServiceState::GivingUp:
            out.running = false;
            out.ready = false;
            break;
        }
    }
    out.crashCount = nonNegative(json.value(QStringLiteral("crashCount")).toInteger());
    out.pid = nonNegative(json.value(QStringLiteral("pid")).toInteger());
    out.updatedAtMs = nonNegative(json.value(QStringLiteral("updatedAtMs")).toInteger());
    if (out.reason.isEmpty()) {
        out.reason = json.value(QStringLiteral("reason")).toString().trimmed();
    }
    return out;
}

inline QJsonObject healthComponentToJson(const HealthComponentV2& component)
{
    QJsonObject out;
    const QString state = component.state.trimmed();
    out[QStringLiteral("state")] =
        state.isEmpty() ? QStringLiteral("unavailable") : state;
    out[QStringLiteral("reason")] = component.reason.trimmed();
    out[QStringLiteral("lastUpdatedMs")] = nonNegative(component.lastUpdatedMs);
    out[QStringLiteral("stalenessMs")] = nonNegative(component.stalenessMs);
    out[QStringLiteral("metrics")] = component.metrics;
    return out;
}

} // namespace bs
