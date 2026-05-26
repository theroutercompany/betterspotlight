#pragma once

#include "core/ipc/message.h"
#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>

namespace bs {

class SocketServer : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxReadBufferSize = 64 * 1024 * 1024; // 64 MB

    class RequestResponder {
    public:
        RequestResponder() = default;

        bool isValid() const;
        bool send(const QJsonObject& response) const;

    private:
        struct ConnectionState {
            std::atomic<bool> connected{true};
        };

        struct State {
            QPointer<SocketServer> server;
            QPointer<QLocalSocket> client;
            std::shared_ptr<ConnectionState> connection;
            std::atomic<bool> completed{false};
            bool hasRequestId = false;
            uint64_t requestId = 0;
        };

        explicit RequestResponder(std::shared_ptr<State> state);
        static bool stateConnected(const std::shared_ptr<State>& state);

        std::shared_ptr<State> m_state;

        friend class SocketServer;
    };

    explicit SocketServer(QObject* parent = nullptr);
    ~SocketServer() override;

    using LegacyRequestHandler = std::function<QJsonObject(const QJsonObject& request)>;
    using RequestHandler =
        std::function<void(const QJsonObject& request, RequestResponder responder)>;

    // Start listening on the given socket path
    bool listen(const QString& socketPath);
    void close();
    bool isListening() const;

    // Set the handler for incoming requests
    void setRequestHandler(LegacyRequestHandler handler);
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
    QMap<QLocalSocket*, std::shared_ptr<RequestResponder::ConnectionState>> m_connectionStates;
    bool m_closing = false;

    bool detachClient(QLocalSocket* client);
    void processBuffer(QLocalSocket* client);
    bool sendResponse(QLocalSocket* client, const QJsonObject& response);
    void sendDeferredResponse(const std::shared_ptr<RequestResponder::State>& state,
                              const QJsonObject& response);
};

} // namespace bs
