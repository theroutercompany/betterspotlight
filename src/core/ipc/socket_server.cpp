#include "core/ipc/socket_server.h"
#include "core/shared/logging.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace bs {

namespace {

bool socketHasActivePeer(const QString& socketPath)
{
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    const bool connected = probe.waitForConnected(150);
    if (connected) {
        probe.disconnectFromServer();
        probe.waitForDisconnected(50);
    }
    return connected;
}

} // namespace

SocketServer::SocketServer(QObject* parent)
    : QObject(parent)
    , m_server(std::make_unique<QLocalServer>(this))
{
    connect(m_server.get(), &QLocalServer::newConnection,
            this, &SocketServer::onNewConnection);
}

SocketServer::~SocketServer()
{
    close();
}

bool SocketServer::listen(const QString& socketPath)
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    if (!m_server->listen(socketPath)) {
        const auto serverError = m_server->serverError();
        const bool addressInUse =
            serverError == QAbstractSocket::AddressInUseError;

        if (addressInUse) {
            if (socketHasActivePeer(socketPath)) {
                const QString err = QStringLiteral(
                    "Socket already in use by an active service: %1").arg(socketPath);
                qCCritical(bsIpc, "%s", qPrintable(err));
                emit errorOccurred(err);
                return false;
            }

            qCWarning(bsIpc, "Detected stale socket, attempting safe cleanup: %s",
                      qPrintable(socketPath));
            QLocalServer::removeServer(socketPath);
            if (!m_server->listen(socketPath)) {
                const QString err = m_server->errorString();
                qCCritical(bsIpc, "Failed to listen on %s after stale cleanup: %s",
                           qPrintable(socketPath), qPrintable(err));
                emit errorOccurred(err);
                return false;
            }
        } else {
            const QString err = m_server->errorString();
            qCCritical(bsIpc, "Failed to listen on %s: %s",
                       qPrintable(socketPath), qPrintable(err));
            emit errorOccurred(err);
            return false;
        }
    }

    qCInfo(bsIpc, "Listening on %s", qPrintable(socketPath));
    return true;
}

void SocketServer::close()
{
    if (m_closing) {
        return;
    }
    m_closing = true;

    // Two-phase shutdown: detach client bookkeeping first, then disconnect sockets.
    const QList<QLocalSocket*> clients = m_clients;
    m_clients.clear();
    m_readBuffers.clear();

    for (QLocalSocket* client : clients) {
        if (!client) {
            continue;
        }
        client->disconnect(this);
        if (client->state() != QLocalSocket::UnconnectedState) {
            client->disconnectFromServer();
        }
        client->deleteLater();
    }

    if (m_server->isListening()) {
        QString path = m_server->fullServerName();
        m_server->close();
        qCInfo(bsIpc, "Server closed: %s", qPrintable(path));
    }

    m_closing = false;
}

bool SocketServer::isListening() const
{
    return m_server->isListening();
}

void SocketServer::setRequestHandler(RequestHandler handler)
{
    m_handler = std::move(handler);
}

void SocketServer::broadcast(const QJsonObject& notification)
{
    QByteArray encoded = IpcMessage::encode(notification);
    if (encoded.isEmpty()) {
        qCWarning(bsIpc, "Failed to encode broadcast notification");
        return;
    }

    for (auto* client : m_clients) {
        client->write(encoded);
        client->flush();
    }

    qCDebug(bsIpc, "Broadcast notification to %d client(s)", static_cast<int>(m_clients.size()));
}

void SocketServer::onNewConnection()
{
    while (QLocalSocket* client = m_server->nextPendingConnection()) {
        qCInfo(bsIpc, "Client connected (fd=%lld)",
               static_cast<long long>(client->socketDescriptor()));

        m_clients.append(client);
        m_readBuffers[client] = QByteArray();

        connect(client, &QLocalSocket::readyRead,
                this, &SocketServer::onClientReadyRead);
        connect(client, &QLocalSocket::disconnected,
                this, &SocketServer::onClientDisconnected);

        emit clientConnected();
    }
}

void SocketServer::onClientReadyRead()
{
    auto* client = qobject_cast<QLocalSocket*>(sender());
    if (!client || !m_readBuffers.contains(client)) return;

    m_readBuffers[client].append(client->readAll());

    if (m_readBuffers[client].size() > kMaxReadBufferSize) {
        qCCritical(bsIpc, "Client read buffer exceeded %d bytes, disconnecting client",
                   kMaxReadBufferSize);
        const bool wasTracked = detachClient(client);
        client->disconnectFromServer();
        if (wasTracked) {
            client->deleteLater();
            emit clientDisconnected();
        }
        return;
    }

    processBuffer(client);
}

void SocketServer::onClientDisconnected()
{
    auto* client = qobject_cast<QLocalSocket*>(sender());
    if (!client) return;

    if (detachClient(client)) {
        qCInfo(bsIpc, "Client disconnected");
        client->deleteLater();
        emit clientDisconnected();
    } else {
        qCDebug(bsIpc, "Ignoring duplicate disconnect callback for client");
    }
}

bool SocketServer::detachClient(QLocalSocket* client)
{
    if (!client) {
        return false;
    }

    const bool removedClient = m_clients.removeOne(client);
    const bool removedBuffer = m_readBuffers.remove(client) > 0;
    return removedClient || removedBuffer;
}

void SocketServer::processBuffer(QLocalSocket* client)
{
    if (!m_readBuffers.contains(client)) {
        return;
    }

    QByteArray& buffer = m_readBuffers[client];

    while (true) {
        auto result = IpcMessage::decode(buffer);
        if (!result) break;

        buffer.remove(0, result->bytesConsumed);

        const QJsonObject& incoming = result->json;
        QString type = incoming.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("request")) {
            qCDebug(bsIpc, "Received request: method=%s id=%lld",
                    qPrintable(incoming.value(QStringLiteral("method")).toString()),
                    incoming.value(QStringLiteral("id")).toInteger());

            QJsonObject response;
            if (m_handler) {
                response = m_handler(incoming);
            } else {
                uint64_t id = static_cast<uint64_t>(incoming.value(QStringLiteral("id")).toInteger());
                response = IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                                  QStringLiteral("No request handler registered"));
            }

            QByteArray encoded = IpcMessage::encode(response);
            if (!encoded.isEmpty()) {
                client->write(encoded);
                client->flush();
            }
        } else if (type == QLatin1String("notification")) {
            qCDebug(bsIpc, "Received notification: method=%s",
                    qPrintable(incoming.value(QStringLiteral("method")).toString()));

            // Notifications are fire-and-forget; pass to handler but discard result
            if (m_handler) {
                m_handler(incoming);
            }
        } else {
            qCWarning(bsIpc, "Received unknown message type: %s", qPrintable(type));
        }
    }
}

} // namespace bs
