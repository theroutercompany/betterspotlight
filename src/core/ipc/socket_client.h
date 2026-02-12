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

    static constexpr int kMaxReadBufferSize = 64 * 1024 * 1024; // 64 MB

    bool connectToServer(const QString& socketPath, int timeoutMs = 5000);
    void disconnect();
    bool isConnected() const;

    // Send a request and get the response (blocking with timeout)
    std::optional<QJsonObject> sendRequest(const QString& method,
                                            const QJsonObject& params = {},
                                            int timeoutMs = 30000);

    using RequestCallback = std::function<void(const std::optional<QJsonObject>& response)>;
    void sendRequestAsync(const QString& method,
                          const QJsonObject& params,
                          int timeoutMs,
                          RequestCallback callback);

    // Send a notification (no response expected)
    bool sendNotification(const QString& method, const QJsonObject& params = {});

    // Async notification handler
    using NotificationHandler = std::function<void(const QString& method, const QJsonObject& params)>;
    void setNotificationHandler(NotificationHandler handler);

    // Enable auto-reconnect with exponential backoff.
    // When the connection drops, the client will try to reconnect
    // up to maxAttempts times with backoff starting at baseDelayMs.
    void enableAutoReconnect(const QString& socketPath,
                             int maxAttempts = 5,
                             int baseDelayMs = 500);
    void disableAutoReconnect();

signals:
    void disconnected();
    void errorOccurred(const QString& error);
    void reconnected();

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

    // Auto-reconnect state
    bool m_autoReconnectEnabled = false;
    QString m_reconnectSocketPath;
    int m_reconnectMaxAttempts = 5;
    int m_reconnectBaseDelayMs = 500;
    int m_reconnectAttempt = 0;

    void attemptReconnect();
};

} // namespace bs
