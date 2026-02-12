#pragma once

#include "app/control_plane/health_snapshot_v2.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <memory>

namespace bs {

class SocketClient;

class HealthAggregatorActor : public QObject {
    Q_OBJECT
public:
    explicit HealthAggregatorActor(QObject* parent = nullptr);
    ~HealthAggregatorActor() override;

    Q_INVOKABLE void initialize(const QString& instanceId);
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void triggerRefresh();
    Q_INVOKABLE void setManagedServices(const QJsonArray& services);

signals:
    void snapshotUpdated(const QJsonObject& snapshot);

private:
    void refreshNow();
    QJsonObject fetchQueryHealth();
    QJsonObject fetchIndexerQueue();
    bool isManagedServiceReady(const QString& serviceName) const;
    QString managedServiceState(const QString& serviceName) const;
    static QString computeOverallState(const QJsonArray& services,
                                       const QJsonObject& mergedHealth,
                                       qint64 stalenessMs,
                                       QString* reason);

    QTimer m_pollTimer;
    QTimer m_eventDebounceTimer;
    QString m_instanceId;
    QJsonArray m_managedServices;
    qint64 m_lastSnapshotTimeMs = 0;
    bool m_running = false;

    std::unique_ptr<SocketClient> m_queryClient;
};

} // namespace bs
