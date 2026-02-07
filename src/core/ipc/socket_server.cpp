#include "core/ipc/socket_server.h"
#include "core/shared/logging.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace bs {

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
    // Remove stale socket file if it exists
    if (QFile::exists(socketPath)) {
        qCInfo(bsIpc, "Removing stale socket: %s", qPrintable(socketPath));
        QFile::remove(socketPath);
    }

    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    if (!m_server->listen(socketPath)) {
        QString err = m_server->errorString();
        qCCritical(bsIpc, "Failed to listen on %s: %s",
                   qPrintable(socketPath), qPrintable(err));
        emit errorOccurred(err);
        return false;
    }

    qCInfo(bsIpc, "Listening on %s", qPrintable(socketPath));
    return true;
}

void SocketServer::close()
{
    // Disconnect and clean up all clients
    for (auto* client : m_clients) {
        client->disconnect(this);
        client->disconnectFromServer();
        client->deleteLater();
    }
    m_clients.clear();
    m_readBuffers.clear();

    if (m_server->isListening()) {
        QString path = m_server->fullServerName();
        m_server->close();
        qCInfo(bsIpc, "Server closed: %s", qPrintable(path));
    }
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
    if (!client) return;

    m_readBuffers[client].append(client->readAll());
    processBuffer(client);
}

void SocketServer::onClientDisconnected()
{
    auto* client = qobject_cast<QLocalSocket*>(sender());
    if (!client) return;

    qCInfo(bsIpc, "Client disconnected");

    m_clients.removeOne(client);
    m_readBuffers.remove(client);
    client->deleteLater();

    emit clientDisconnected();
}

void SocketServer::processBuffer(QLocalSocket* client)
{
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
