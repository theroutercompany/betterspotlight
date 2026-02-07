#pragma once

#include "core/ipc/message.h"
#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <functional>
#include <memory>

namespace bs {

class SocketServer : public QObject {
    Q_OBJECT
public:
    explicit SocketServer(QObject* parent = nullptr);
    ~SocketServer() override;

    using RequestHandler = std::function<QJsonObject(const QJsonObject& request)>;

    // Start listening on the given socket path
    bool listen(const QString& socketPath);
    void close();
    bool isListening() const;

    // Set the handler for incoming requests
    void setRequestHandler(RequestHandler handler);

    // Broadcast a notification to all connected clients
    void broadcast(const QJsonObject& notification);

signals:
    void clientConnected();
    void clientDisconnected();
    void errorOccurred(const QString& error);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();

private:
    std::unique_ptr<QLocalServer> m_server;
    QList<QLocalSocket*> m_clients;
    RequestHandler m_handler;
    QMap<QLocalSocket*, QByteArray> m_readBuffers;

    void processBuffer(QLocalSocket* client);
};

} // namespace bs
