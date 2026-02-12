#include "inference_supervisor_actor.h"

#include <QRandomGenerator>

#include <algorithm>

namespace bs {

InferenceSupervisorActor::InferenceSupervisorActor(QObject* parent)
    : QObject(parent)
{
}

InferenceSupervisorActor::~InferenceSupervisorActor() = default;

InferenceSupervisorActor::RecoveryDecision InferenceSupervisorActor::recordFailure(const QString& role)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RoleState& state = m_roleStates[role];
    state.consecutiveFailures += 1;
    state.available = false;

    RecoveryDecision decision;
    decision.consecutiveFailures = state.consecutiveFailures;

    if (state.consecutiveFailures < kRestartThreshold) {
        decision.restartRequested = false;
        decision.givingUp = false;
        decision.restartAttempts = state.restartAttempts;
        decision.backoffMs = state.backoffMs;
        return decision;
    }

    if (state.restartAttempts >= kRestartBudget) {
        state.givingUp = true;
        decision.restartRequested = false;
        decision.givingUp = true;
        decision.restartAttempts = state.restartAttempts;
        decision.backoffMs = state.backoffMs;
        return decision;
    }

    state.restartAttempts += 1;
    state.backoffMs = jitterMs(computeBackoffMs(state.restartAttempts));

    decision.restartRequested = true;
    decision.givingUp = false;
    decision.backoffMs = state.backoffMs;
    decision.restartAttempts = state.restartAttempts;
    return decision;
}

void InferenceSupervisorActor::recordSuccess(const QString& role)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RoleState& state = m_roleStates[role];
    state.consecutiveFailures = 0;
    state.givingUp = false;
    state.available = true;
}

void InferenceSupervisorActor::recordTimeout(const QString& role)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RoleState& state = m_roleStates[role];
    state.consecutiveFailures = 0;
    state.available = true;
}

void InferenceSupervisorActor::markRoleUnavailable(const QString& role)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RoleState& state = m_roleStates[role];
    state.available = false;
}

void InferenceSupervisorActor::resetRole(const QString& role)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RoleState& state = m_roleStates[role];
    state.consecutiveFailures = 0;
    state.backoffMs = 0;
    state.givingUp = false;
    state.available = true;
}

QJsonObject InferenceSupervisorActor::supervisorStateByRole() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QJsonObject out;
    for (auto it = m_roleStates.constBegin(); it != m_roleStates.constEnd(); ++it) {
        const RoleState& state = it.value();
        if (state.givingUp) {
            out[it.key()] = QStringLiteral("giving_up");
        } else if (!state.available) {
            out[it.key()] = QStringLiteral("degraded");
        } else {
            out[it.key()] = QStringLiteral("ready");
        }
    }
    return out;
}

QJsonObject InferenceSupervisorActor::backoffMsByRole() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QJsonObject out;
    for (auto it = m_roleStates.constBegin(); it != m_roleStates.constEnd(); ++it) {
        out[it.key()] = it.value().backoffMs;
    }
    return out;
}

QJsonObject InferenceSupervisorActor::restartCountByRole() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QJsonObject out;
    for (auto it = m_roleStates.constBegin(); it != m_roleStates.constEnd(); ++it) {
        out[it.key()] = it.value().restartAttempts;
    }
    return out;
}

QJsonObject InferenceSupervisorActor::restartBudgetExhaustedByRole() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QJsonObject out;
    for (auto it = m_roleStates.constBegin(); it != m_roleStates.constEnd(); ++it) {
        out[it.key()] = it.value().givingUp;
    }
    return out;
}

int InferenceSupervisorActor::computeBackoffMs(int restartAttempts)
{
    const int exponent = std::max(0, restartAttempts - 1);
    const int raw = 250 * (1 << std::min(exponent, 16));
    return std::min(raw, 30000);
}

int InferenceSupervisorActor::jitterMs(int baseMs)
{
    const int jitterCap = std::max(1, static_cast<int>(baseMs * 0.2));
    const int jitter = QRandomGenerator::global()->bounded(jitterCap + 1);
    return baseMs + jitter;
}

} // namespace bs
