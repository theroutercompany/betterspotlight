#pragma once

#include "app/control_plane/control_plane_types.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <memory>

namespace bs {

class Supervisor;

class ControlPlaneActor : public QObject {
    Q_OBJECT
public:
    explicit ControlPlaneActor(QObject* parent = nullptr);
    ~ControlPlaneActor() override;

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void configureServices(const QVariantList& serviceDescriptors);
    Q_INVOKABLE bool startAll();
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE bool restartService(const QString& serviceName,
                                    const QString& reason = QStringLiteral("manual"));
    Q_INVOKABLE void shutdown(const QString& reason = QStringLiteral("app_shutdown"));
    Q_INVOKABLE void setLifecyclePhase(const QString& phase);
    Q_INVOKABLE QString lifecyclePhase() const;
    Q_INVOKABLE QJsonArray serviceSnapshotSync() const;
    Q_INVOKABLE QJsonObject sendServiceRequestSync(const QString& serviceName,
                                                   const QString& method,
                                                   const QJsonObject& params = {},
                                                   int timeoutMs = 10000);

signals:
    void lifecyclePhaseChanged(const QString& phase);
    void serviceStatusChanged(const QString& name, const QString& status);
    void serviceError(const QString& serviceName, const QString& error);
    void allServicesReady();
    void managedServicesUpdated(const QJsonArray& services);

private slots:
    void onSupervisorServiceStarted(const QString& name);
    void onSupervisorServiceStopped(const QString& name);
    void onSupervisorServiceCrashed(const QString& name, int crashCount);
    void onSupervisorServiceStateChanged(const QString& name, const QString& state);
    void onSupervisorAllReady();

private:
    void publishSnapshot();
    void updateServiceState(const QString& name, const QString& status);
    bool ensureSupervisorInitialized();
    QString makeCommandKey(const QString& service, const QString& verb, const QString& reason) const;
    bool beginCommand(const QString& service,
                      const QString& verb,
                      const QString& reason,
                      QString* commandKeyOut);
    void endCommand(const QString& commandKey);

    std::unique_ptr<Supervisor> m_supervisor;
    QHash<QString, QString> m_serviceStates;
    QHash<QString, qint64> m_recentCommandMs;
    QString m_activeCommandKey;
    AppLifecyclePhase m_lifecyclePhase = AppLifecyclePhase::Starting;
    bool m_servicesConfigured = false;
    bool m_started = false;
    bool m_stopping = false;

    static constexpr qint64 kCommandDedupeWindowMs = 750;
};

} // namespace bs
