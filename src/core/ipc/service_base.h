#pragma once

#include "core/ipc/socket_server.h"
#include <QCoreApplication>
#include <QString>
#include <functional>

namespace bs {

class ServiceBase : public QObject {
    Q_OBJECT
public:
    explicit ServiceBase(const QString& serviceName, QObject* parent = nullptr);
    ~ServiceBase() override;

    // Run the service (enters event loop, returns exit code)
    int run();

    // Get the socket path for this service
    static QString socketPath(const QString& serviceName);
    static QString runtimeDirectory();
    static QString socketDirectory();
    static QString pidDirectory();
    static QString pidPath(const QString& serviceName);

protected:
    // Override to handle specific methods
    virtual QJsonObject handleRequest(const QJsonObject& request);

    // Built-in handlers
    QJsonObject handlePing(const QJsonObject& request);
    QJsonObject handleShutdown(const QJsonObject& request);

    // Send a notification to connected clients
    void sendNotification(const QString& method, const QJsonObject& params = {});

    QString m_serviceName;
    std::unique_ptr<SocketServer> m_server;
};

} // namespace bs
