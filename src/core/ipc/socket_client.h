#pragma once

#include "core/ipc/message.h"
#include <QObject>
#include <QLocalSocket>
#include <functional>
#include <memory>

namespace bs {

class SocketClient : public QObject {
    Q_OBJECT
public:
    explicit SocketClient(QObject* parent = nullptr);
    ~SocketClient() override;

    bool connectToServer(const QString& socketPath, int timeoutMs = 5000);
    void disconnect();
    bool isConnected() const;

    // Send a request and get the response (blocking with timeout)
    std::optional<QJsonObject> sendRequest(const QString& method,
                                            const QJsonObject& params = {},
                                            int timeoutMs = 30000);

    // Send a notification (no response expected)
    bool sendNotification(const QString& method, const QJsonObject& params = {});

    // Async notification handler
    using NotificationHandler = std::function<void(const QString& method, const QJsonObject& params)>;
    void setNotificationHandler(NotificationHandler handler);

signals:
    void disconnected();
    void errorOccurred(const QString& error);

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    std::unique_ptr<QLocalSocket> m_socket;
    QByteArray m_readBuffer;
    uint64_t m_nextRequestId = 1;
    NotificationHandler m_notificationHandler;

    struct PendingRequest {
        QJsonObject response;
        bool completed = false;
    };
    QMap<uint64_t, std::shared_ptr<PendingRequest>> m_pending;
};

} // namespace bs
