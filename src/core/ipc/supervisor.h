#pragma once

#include "core/ipc/socket_client.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <vector>

namespace bs {

struct ServiceInfo {
    QString name;
    QString executablePath;
    int crashCount = 0;
    int64_t lastCrashTime = 0;
    int64_t firstCrashTime = 0;
};

class Supervisor : public QObject {
    Q_OBJECT
public:
    explicit Supervisor(QObject* parent = nullptr);
    ~Supervisor() override;

    // Add a service to manage
    void addService(const QString& name, const QString& executablePath);

    // Start all services
    bool startAll();

    // Stop all services gracefully
    void stopAll();

    // Get client for a specific service
    SocketClient* clientFor(const QString& serviceName);

signals:
    void serviceStarted(const QString& name);
    void serviceStopped(const QString& name);
    void serviceCrashed(const QString& name, int crashCount);
    void allServicesReady();

private slots:
    void onServiceFinished(int exitCode, QProcess::ExitStatus status);
    void heartbeat();

private:
    struct ManagedService {
        ServiceInfo info;
        std::unique_ptr<QProcess> process;
        std::unique_ptr<SocketClient> client;
        bool ready = false;
    };

    std::vector<std::unique_ptr<ManagedService>> m_services;
    std::unique_ptr<QTimer> m_heartbeatTimer;

    static constexpr int kHeartbeatIntervalMs = 10000;
    static constexpr int kMaxCrashesBeforeGiveUp = 3;
    static constexpr int kCrashWindowSeconds = 60;

    void startService(ManagedService& svc);
    void restartService(ManagedService& svc);
    int restartDelayMs(int crashCount) const;
    void createSocketDirectory();
    ManagedService* findService(const QString& name);
};

} // namespace bs
