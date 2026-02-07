#include "core/ipc/message.h"
#include "core/shared/logging.h"
#include <QJsonDocument>
#include <QtEndian>

namespace bs {

QByteArray IpcMessage::encode(const QJsonObject& json)
{
    QJsonDocument doc(json);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    if (payload.size() > kMaxMessageSize) {
        qCWarning(bsIpc, "Message exceeds max size: %d > %d",
                  static_cast<int>(payload.size()), kMaxMessageSize);
        return {};
    }

    QByteArray msg;
    msg.reserve(4 + payload.size());

    // 4-byte big-endian length prefix
    quint32 len = qToBigEndian(static_cast<quint32>(payload.size()));
    msg.append(reinterpret_cast<const char*>(&len), 4);
    msg.append(payload);

    return msg;
}

std::optional<IpcMessage::DecodeResult> IpcMessage::decode(const QByteArray& buffer)
{
    // Need at least 4 bytes for the length prefix
    if (buffer.size() < 4) {
        return std::nullopt;
    }

    // Read the 4-byte big-endian length
    quint32 rawLen;
    memcpy(&rawLen, buffer.constData(), 4);
    quint32 payloadLen = qFromBigEndian(rawLen);

    if (static_cast<int>(payloadLen) > kMaxMessageSize) {
        qCWarning(bsIpc, "Received message length exceeds max: %u > %d",
                  payloadLen, kMaxMessageSize);
        return std::nullopt;
    }

    // Check if the full payload has arrived
    int totalLen = 4 + static_cast<int>(payloadLen);
    if (buffer.size() < totalLen) {
        return std::nullopt;
    }

    // Parse the JSON payload
    QByteArray payload = buffer.mid(4, static_cast<int>(payloadLen));
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(bsIpc, "JSON parse error: %s",
                  qPrintable(parseError.errorString()));
        return std::nullopt;
    }

    if (!doc.isObject()) {
        qCWarning(bsIpc, "Expected JSON object, got something else");
        return std::nullopt;
    }

    DecodeResult result;
    result.json = doc.object();
    result.bytesConsumed = totalLen;
    return result;
}

QJsonObject IpcMessage::makeRequest(uint64_t id, const QString& method, const QJsonObject& params)
{
    QJsonObject json;
    json[QStringLiteral("type")] = QStringLiteral("request");
    json[QStringLiteral("id")] = static_cast<qint64>(id);
    json[QStringLiteral("method")] = method;
    if (!params.isEmpty()) {
        json[QStringLiteral("params")] = params;
    }
    return json;
}

QJsonObject IpcMessage::makeResponse(uint64_t id, const QJsonObject& result)
{
    QJsonObject json;
    json[QStringLiteral("type")] = QStringLiteral("response");
    json[QStringLiteral("id")] = static_cast<qint64>(id);
    json[QStringLiteral("result")] = result;
    return json;
}

QJsonObject IpcMessage::makeError(uint64_t id, IpcErrorCode code, const QString& message)
{
    QJsonObject errorObj;
    errorObj[QStringLiteral("code")] = static_cast<int>(code);
    errorObj[QStringLiteral("codeString")] = ipcErrorCodeToString(code);
    errorObj[QStringLiteral("message")] = message;

    QJsonObject json;
    json[QStringLiteral("type")] = QStringLiteral("error");
    json[QStringLiteral("id")] = static_cast<qint64>(id);
    json[QStringLiteral("error")] = errorObj;
    return json;
}

QJsonObject IpcMessage::makeNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject json;
    json[QStringLiteral("type")] = QStringLiteral("notification");
    json[QStringLiteral("method")] = method;
    if (!params.isEmpty()) {
        json[QStringLiteral("params")] = params;
    }
    return json;
}

} // namespace bs
