#include "core/ipc/socket_client.h"
#include "core/shared/logging.h"
#include <QPointer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>

namespace bs {

namespace {

bool isTransientConnectError(QLocalSocket::LocalSocketError error)
{
    switch (error) {
    case QLocalSocket::ServerNotFoundError:
    case QLocalSocket::ConnectionRefusedError:
    case QLocalSocket::SocketTimeoutError:
        return true;
    default:
        return false;
    }
}

} // namespace

SocketClient::SocketClient(QObject* parent)
    : QObject(parent)
    , m_socket(std::make_unique<QLocalSocket>(this))
{
    connect(m_socket.get(), &QLocalSocket::readyRead,
            this, &SocketClient::onReadyRead);
    connect(m_socket.get(), &QLocalSocket::disconnected,
            this, &SocketClient::onDisconnected);
}

SocketClient::~SocketClient()
{
    disconnect();
}

bool SocketClient::connectToServer(const QString& socketPath, int timeoutMs)
{
    const QString normalizedSocketPath = socketPath.trimmed();
    if (normalizedSocketPath.isEmpty()) {
        const QString err = QStringLiteral("Invalid socket path: empty");
        qCCritical(bsIpc, "%s", qPrintable(err));
        emit errorOccurred(err);
        return false;
    }

    if (timeoutMs <= 0) {
        const QString err = QStringLiteral("Invalid connect timeout: %1ms").arg(timeoutMs);
        qCCritical(bsIpc, "%s", qPrintable(err));
        emit errorOccurred(err);
        return false;
    }

    if (m_socket->state() == QLocalSocket::ConnectedState
        && m_socket->serverName() == normalizedSocketPath) {
        return true;
    }

    failAllPendingRequests(QStringLiteral("Connection reset"));

    // Always abort before reconnect attempts to clear stale state.
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_socket->abort();
    m_readBuffer.clear();

    qCDebug(bsIpc, "Connecting to %s (timeout=%dms)", qPrintable(normalizedSocketPath), timeoutMs);

    m_socket->connectToServer(normalizedSocketPath);
    if (!m_socket->waitForConnected(timeoutMs)) {
        const auto error = m_socket->error();
        const QString err = m_socket->errorString();
        if (isTransientConnectError(error)) {
            qCDebug(bsIpc, "Service not ready at %s yet: %s",
                    qPrintable(normalizedSocketPath), qPrintable(err));
        } else {
            qCCritical(bsIpc, "Hard connect failure for %s: %s (error=%d)",
                       qPrintable(normalizedSocketPath), qPrintable(err), static_cast<int>(error));
            emit errorOccurred(err);
        }
        return false;
    }

    qCInfo(bsIpc, "Connected to %s", qPrintable(normalizedSocketPath));
    return true;
}

void SocketClient::disconnect()
{
    failAllPendingRequests(QStringLiteral("Disconnected"));
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->disconnectFromServer();
    }
    m_readBuffer.clear();
}

bool SocketClient::isConnected() const
{
    return m_socket->state() == QLocalSocket::ConnectedState;
}

std::optional<QJsonObject> SocketClient::sendRequest(const QString& method,
                                                      const QJsonObject& params,
                                                      int timeoutMs)
{
    if (!isConnected()) {
        qCWarning(bsIpc, "Cannot send request: not connected");
        return std::nullopt;
    }

    uint64_t id = m_nextRequestId++;
    QJsonObject request = IpcMessage::makeRequest(id, method, params);
    QByteArray encoded = IpcMessage::encode(request);

    if (encoded.isEmpty()) {
        qCWarning(bsIpc, "Failed to encode request for method=%s", qPrintable(method));
        return std::nullopt;
    }

    qCDebug(bsIpc, "Sending request: method=%s id=%llu", qPrintable(method),
            static_cast<unsigned long long>(id));

    auto pending = std::make_shared<PendingRequest>();
    m_pending[id] = pending;

    // Write the message
    m_socket->write(encoded);
    m_socket->flush();

    // Block-wait for response without pumping the global event loop.
    // This avoids re-entrancy side effects when callers invoke synchronous RPC on UI paths.
    QElapsedTimer timer;
    timer.start();

    while (!pending->completed && timer.elapsed() < timeoutMs) {
        const int remainingMs = std::max(0, timeoutMs - static_cast<int>(timer.elapsed()));
        if (remainingMs == 0) {
            break;
        }

        if (m_socket->bytesAvailable() == 0) {
            const int waitMs = std::min(remainingMs, 50);
            m_socket->waitForReadyRead(waitMs);
        }

        if (m_socket->bytesAvailable() > 0 || m_socket->state() != QLocalSocket::ConnectedState) {
            onReadyRead();
        }
    }

    m_pending.remove(id);

    if (!pending->completed) {
        qCWarning(bsIpc, "Request timed out: method=%s id=%llu timeout=%dms",
                  qPrintable(method), static_cast<unsigned long long>(id), timeoutMs);
        return std::nullopt;
    }

    return pending->response;
}

void SocketClient::sendRequestAsync(const QString& method,
                                    const QJsonObject& params,
                                    int timeoutMs,
                                    RequestCallback callback)
{
    if (!callback) {
        return;
    }
    if (!isConnected()) {
        qCWarning(bsIpc, "Cannot send async request: not connected");
        callback(std::nullopt);
        return;
    }

    const int effectiveTimeoutMs = std::max(1, timeoutMs);
    uint64_t id = m_nextRequestId++;
    QJsonObject request = IpcMessage::makeRequest(id, method, params);
    QByteArray encoded = IpcMessage::encode(request);

    if (encoded.isEmpty()) {
        qCWarning(bsIpc, "Failed to encode async request for method=%s", qPrintable(method));
        callback(std::nullopt);
        return;
    }

    auto pending = std::make_shared<PendingRequest>();
    pending->asynchronous = true;
    pending->callback = std::move(callback);

    auto* timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    pending->timeoutTimer = timeoutTimer;

    QPointer<SocketClient> self(this);
    connect(timeoutTimer, &QTimer::timeout, this, [self, id, pending]() {
        if (!self) {
            return;
        }
        auto it = self->m_pending.find(id);
        if (it == self->m_pending.end() || it.value().get() != pending.get()) {
            return;
        }

        qCWarning(bsIpc, "Async request timed out: id=%llu timeout=%dms",
                  static_cast<unsigned long long>(id),
                  pending->timeoutTimer ? pending->timeoutTimer->interval() : 0);
        self->completePendingRequest(id, pending, std::nullopt);
    });

    m_pending[id] = pending;

    qCDebug(bsIpc, "Sending async request: method=%s id=%llu",
            qPrintable(method),
            static_cast<unsigned long long>(id));
    m_socket->write(encoded);
    m_socket->flush();
    timeoutTimer->start(effectiveTimeoutMs);
}

bool SocketClient::sendNotification(const QString& method, const QJsonObject& params)
{
    if (!isConnected()) {
        qCWarning(bsIpc, "Cannot send notification: not connected");
        return false;
    }

    QJsonObject notification = IpcMessage::makeNotification(method, params);
    QByteArray encoded = IpcMessage::encode(notification);

    if (encoded.isEmpty()) {
        qCWarning(bsIpc, "Failed to encode notification for method=%s", qPrintable(method));
        return false;
    }

    qCDebug(bsIpc, "Sending notification: method=%s", qPrintable(method));

    m_socket->write(encoded);
    m_socket->flush();
    return true;
}

void SocketClient::setNotificationHandler(NotificationHandler handler)
{
    m_notificationHandler = std::move(handler);
}

void SocketClient::onReadyRead()
{
    m_readBuffer.append(m_socket->readAll());

    if (m_readBuffer.size() > kMaxReadBufferSize) {
        qCCritical(bsIpc, "Read buffer exceeded %d bytes, disconnecting", kMaxReadBufferSize);
        m_readBuffer.clear();
        m_socket->disconnectFromServer();
        return;
    }

    while (true) {
        auto result = IpcMessage::decode(m_readBuffer);
        if (!result) break;

        m_readBuffer.remove(0, result->bytesConsumed);

        const QJsonObject& msg = result->json;
        QString type = msg.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("response") || type == QLatin1String("error")) {
            uint64_t id = static_cast<uint64_t>(msg.value(QStringLiteral("id")).toInteger());

            auto it = m_pending.find(id);
            if (it != m_pending.end()) {
                const auto pending = it.value();
                if (pending->asynchronous) {
                    completePendingRequest(id, pending, msg);
                } else {
                    pending->response = msg;
                    pending->completed = true;
                }
            } else {
                qCDebug(bsIpc, "Ignoring late response for request id=%llu",
                        static_cast<unsigned long long>(id));
            }
        } else if (type == QLatin1String("notification")) {
            QString method = msg.value(QStringLiteral("method")).toString();
            QJsonObject params = msg.value(QStringLiteral("params")).toObject();

            qCDebug(bsIpc, "Received notification: method=%s", qPrintable(method));

            if (m_notificationHandler) {
                m_notificationHandler(method, params);
            }
        } else {
            qCWarning(bsIpc, "Received unexpected message type: %s", qPrintable(type));
        }
    }
}

void SocketClient::onDisconnected()
{
    qCInfo(bsIpc, "Disconnected from server");

    failAllPendingRequests(QStringLiteral("Connection lost"));

    emit disconnected();

    // Attempt auto-reconnect if enabled
    if (m_autoReconnectEnabled) {
        m_reconnectAttempt = 0;
        attemptReconnect();
    }
}

void SocketClient::enableAutoReconnect(const QString& socketPath,
                                        int maxAttempts,
                                        int baseDelayMs)
{
    m_autoReconnectEnabled = true;
    m_reconnectSocketPath = socketPath;
    m_reconnectMaxAttempts = maxAttempts;
    m_reconnectBaseDelayMs = baseDelayMs;
    m_reconnectAttempt = 0;
}

void SocketClient::disableAutoReconnect()
{
    m_autoReconnectEnabled = false;
    m_reconnectAttempt = 0;
}

void SocketClient::attemptReconnect()
{
    if (!m_autoReconnectEnabled) return;

    if (m_reconnectAttempt >= m_reconnectMaxAttempts) {
        qCWarning(bsIpc, "Auto-reconnect exhausted %d attempts for %s",
                  m_reconnectMaxAttempts, qPrintable(m_reconnectSocketPath));
        emit errorOccurred(QStringLiteral("Auto-reconnect failed after %1 attempts")
                               .arg(m_reconnectMaxAttempts));
        return;
    }

    // Exponential backoff: base * 2^attempt (500ms, 1s, 2s, 4s, 8s)
    int delay = m_reconnectBaseDelayMs * (1 << m_reconnectAttempt);
    ++m_reconnectAttempt;

    qCInfo(bsIpc, "Auto-reconnect attempt %d/%d in %dms for %s",
           m_reconnectAttempt, m_reconnectMaxAttempts, delay,
           qPrintable(m_reconnectSocketPath));

    QTimer::singleShot(delay, this, [this]() {
        if (!m_autoReconnectEnabled) return;
        if (isConnected()) return;

        if (connectToServer(m_reconnectSocketPath, 3000)) {
            qCInfo(bsIpc, "Auto-reconnect succeeded on attempt %d", m_reconnectAttempt);
            m_reconnectAttempt = 0;
            emit reconnected();
        } else {
            attemptReconnect();
        }
    });
}

void SocketClient::completePendingRequest(
    uint64_t id,
    const std::shared_ptr<PendingRequest>& pending,
    const std::optional<QJsonObject>& response)
{
    if (!pending) {
        return;
    }

    if (pending->timeoutTimer) {
        pending->timeoutTimer->stop();
        pending->timeoutTimer->deleteLater();
        pending->timeoutTimer = nullptr;
    }

    pending->completed = true;
    if (response.has_value()) {
        pending->response = response.value();
    } else {
        pending->response = QJsonObject();
    }

    const bool removed = m_pending.remove(id) > 0;
    Q_UNUSED(removed);

    if (pending->callback) {
        auto callback = std::move(pending->callback);
        callback(response);
    }
}

void SocketClient::failAllPendingRequests(const QString& reason)
{
    if (m_pending.isEmpty()) {
        return;
    }

    const auto pendingMap = m_pending;
    m_pending.clear();

    for (auto it = pendingMap.constBegin(); it != pendingMap.constEnd(); ++it) {
        const auto& pending = it.value();
        if (!pending) {
            continue;
        }

        if (pending->timeoutTimer) {
            pending->timeoutTimer->stop();
            pending->timeoutTimer->deleteLater();
            pending->timeoutTimer = nullptr;
        }

        pending->completed = true;
        pending->response = IpcMessage::makeError(
            it.key(), IpcErrorCode::ServiceUnavailable, reason);

        if (pending->callback) {
            auto callback = std::move(pending->callback);
            callback(pending->response);
        }
    }
}

} // namespace bs
