#pragma once

#include "core/ipc/service_base.h"
#include "core/ipc/socket_client.h"

#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QString>

namespace bs::test {

struct ServiceLaunchConfig {
    QString homeDir;
    QString dataDir;
    QHash<QString, QString> env;
    bool forwardChannels = true;
    int startTimeoutMs = 5000;
    int connectTimeoutMs = 5000;
    int readyTimeoutMs = 30000;
    int requestDefaultTimeoutMs = 5000;
    bool waitForReadyBanner = true;
    bool requirePingReady = true;
};

class ServiceProcessHarness {
public:
    ServiceProcessHarness(QString serviceName, QString binaryName);
    ~ServiceProcessHarness();

    ServiceProcessHarness(const ServiceProcessHarness&) = delete;
    ServiceProcessHarness& operator=(const ServiceProcessHarness&) = delete;

    bool start(const ServiceLaunchConfig& config = {});
    void stop();

    bool isRunning() const;
    QString socketPath() const;
    QString binaryPath() const;

    SocketClient& client();
    QProcess& process();

    QJsonObject request(const QString& method,
                        const QJsonObject& params = {},
                        int timeoutMs = -1);

private:
    QString m_serviceName;
    QString m_binaryName;
    QString m_binaryPath;

    QTemporaryDir m_socketDir;
    QString m_socketPath;

    QProcess m_process;
    SocketClient m_client;
    bool m_started = false;
    int m_requestDefaultTimeoutMs = 5000;
};

} // namespace bs::test
