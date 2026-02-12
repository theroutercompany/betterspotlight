#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QString>

#include <mutex>

namespace bs {

class InferenceSupervisorActor : public QObject {
    Q_OBJECT
public:
    struct RecoveryDecision {
        bool restartRequested = false;
        bool givingUp = false;
        int backoffMs = 0;
        int restartAttempts = 0;
        int consecutiveFailures = 0;
    };

    explicit InferenceSupervisorActor(QObject* parent = nullptr);
    ~InferenceSupervisorActor() override;

    RecoveryDecision recordFailure(const QString& role);
    void recordSuccess(const QString& role);
    void recordTimeout(const QString& role);
    void markRoleUnavailable(const QString& role);
    void resetRole(const QString& role);

    QJsonObject supervisorStateByRole() const;
    QJsonObject backoffMsByRole() const;
    QJsonObject restartCountByRole() const;
    QJsonObject restartBudgetExhaustedByRole() const;

private:
    struct RoleState {
        int consecutiveFailures = 0;
        int restartAttempts = 0;
        int backoffMs = 0;
        bool givingUp = false;
        bool available = true;
    };

    static int computeBackoffMs(int restartAttempts);
    static int jitterMs(int baseMs);

    mutable std::mutex m_mutex;
    QHash<QString, RoleState> m_roleStates;

    static constexpr int kRestartThreshold = 3;
    static constexpr int kRestartBudget = 4;
};

} // namespace bs
