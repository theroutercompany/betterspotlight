#include "core/ipc/socket_client.h"
#include "core/shared/logging.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

namespace bs {

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
    if (m_socket->state() == QLocalSocket::ConnectedState) {
        return true;
    }

    qCDebug(bsIpc, "Connecting to %s (timeout=%dms)", qPrintable(socketPath), timeoutMs);

    m_socket->connectToServer(socketPath);
    if (!m_socket->waitForConnected(timeoutMs)) {
        QString err = m_socket->errorString();
        qCWarning(bsIpc, "Failed to connect to %s: %s",
                  qPrintable(socketPath), qPrintable(err));
        emit errorOccurred(err);
        return false;
    }

    qCInfo(bsIpc, "Connected to %s", qPrintable(socketPath));
    return true;
}

void SocketClient::disconnect()
{
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->disconnectFromServer();
    }
    m_readBuffer.clear();
    m_pending.clear();
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

    // Register pending request
    PendingRequest pending;
    m_pending[id] = &pending;

    // Write the message
    m_socket->write(encoded);
    m_socket->flush();

    // Block-wait for response, processing events to allow readyRead signals
    QElapsedTimer timer;
    timer.start();

    while (!pending.completed && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        // Also try reading directly in case signals are not delivered
        if (m_socket->bytesAvailable() > 0) {
            onReadyRead();
        }
    }

    m_pending.remove(id);

    if (!pending.completed) {
        qCWarning(bsIpc, "Request timed out: method=%s id=%llu timeout=%dms",
                  qPrintable(method), static_cast<unsigned long long>(id), timeoutMs);
        return std::nullopt;
    }

    return pending.response;
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
                it.value()->response = msg;
                it.value()->completed = true;
            } else {
                qCWarning(bsIpc, "Received response for unknown request id=%llu",
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

    // Mark all pending requests as failed
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        it.value()->completed = true;
        it.value()->response = IpcMessage::makeError(
            it.key(), IpcErrorCode::ServiceUnavailable,
            QStringLiteral("Connection lost"));
    }

    emit disconnected();
}

} // namespace bs
