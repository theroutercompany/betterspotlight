#include "core/ipc/message.h"
#include "core/shared/logging.h"
#include <QJsonDocument>
#include <QJsonValue>
#include <QtEndian>

#include <cmath>
#include <limits>

namespace bs {

namespace {

IpcMessage::DecodeAttempt invalidFrame(const QString& error)
{
    IpcMessage::DecodeAttempt attempt;
    attempt.status = IpcMessage::DecodeStatus::Invalid;
    attempt.error = error;
    qCWarning(bsIpc, "%s", qPrintable(error));
    return attempt;
}

bool isNonNegativeJsonInteger(const QJsonValue& value)
{
    if (!value.isDouble()) {
        return false;
    }
    const double numeric = value.toDouble(-1.0);
    return std::isfinite(numeric)
        && numeric >= 0.0
        && numeric <= static_cast<double>(std::numeric_limits<qint64>::max())
        && std::floor(numeric) == numeric;
}

std::optional<QString> validateMessageShape(const QJsonObject& json)
{
    const QJsonValue typeValue = json.value(QStringLiteral("type"));
    if (!typeValue.isString()) {
        return QStringLiteral("IPC message missing string type");
    }

    const QString type = typeValue.toString();
    if (type == QLatin1String("request")) {
        if (!isNonNegativeJsonInteger(json.value(QStringLiteral("id")))) {
            return QStringLiteral("IPC request missing non-negative integer id");
        }
        if (!json.value(QStringLiteral("method")).isString()
            || json.value(QStringLiteral("method")).toString().isEmpty()) {
            return QStringLiteral("IPC request missing non-empty method");
        }
        const QJsonValue params = json.value(QStringLiteral("params"));
        if (!params.isUndefined() && !params.isObject()) {
            return QStringLiteral("IPC request params must be an object");
        }
        return std::nullopt;
    }

    if (type == QLatin1String("response")) {
        if (!isNonNegativeJsonInteger(json.value(QStringLiteral("id")))) {
            return QStringLiteral("IPC response missing non-negative integer id");
        }
        if (!json.value(QStringLiteral("result")).isObject()) {
            return QStringLiteral("IPC response result must be an object");
        }
        return std::nullopt;
    }

    if (type == QLatin1String("error")) {
        if (!isNonNegativeJsonInteger(json.value(QStringLiteral("id")))) {
            return QStringLiteral("IPC error missing non-negative integer id");
        }
        const QJsonObject error = json.value(QStringLiteral("error")).toObject();
        if (error.isEmpty()) {
            return QStringLiteral("IPC error missing error object");
        }
        if (!error.value(QStringLiteral("code")).isDouble()) {
            return QStringLiteral("IPC error missing numeric code");
        }
        if (!error.value(QStringLiteral("message")).isString()) {
            return QStringLiteral("IPC error missing string message");
        }
        return std::nullopt;
    }

    if (type == QLatin1String("notification")) {
        if (!json.value(QStringLiteral("method")).isString()
            || json.value(QStringLiteral("method")).toString().isEmpty()) {
            return QStringLiteral("IPC notification missing non-empty method");
        }
        const QJsonValue params = json.value(QStringLiteral("params"));
        if (!params.isUndefined() && !params.isObject()) {
            return QStringLiteral("IPC notification params must be an object");
        }
        return std::nullopt;
    }

    return QStringLiteral("IPC message has unknown type: %1").arg(type);
}

} // namespace

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
    DecodeAttempt attempt = decodeFrame(buffer);
    if (attempt.status != DecodeStatus::Complete || !attempt.result.has_value()) {
        return std::nullopt;
    }
    return attempt.result;
}

std::optional<QString> IpcMessage::validate(const QJsonObject& json)
{
    return validateMessageShape(json);
}

IpcMessage::DecodeAttempt IpcMessage::decodeFrame(const QByteArray& buffer)
{
    // Need at least 4 bytes for the length prefix
    if (buffer.size() < 4) {
        return {};
    }

    // Read the 4-byte big-endian length
    quint32 rawLen;
    memcpy(&rawLen, buffer.constData(), 4);
    quint32 payloadLen = qFromBigEndian(rawLen);

    if (payloadLen > static_cast<quint32>(kMaxMessageSize)) {
        return invalidFrame(QStringLiteral("message length exceeds max: %1 > %2")
                                .arg(payloadLen)
                                .arg(kMaxMessageSize));
    }

    // Check if the full payload has arrived
    int totalLen = 4 + static_cast<int>(payloadLen);
    if (buffer.size() < totalLen) {
        return {};
    }

    // Parse the JSON payload
    QByteArray payload = buffer.mid(4, static_cast<int>(payloadLen));
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        return invalidFrame(QStringLiteral("JSON parse error: %1").arg(parseError.errorString()));
    }

    if (!doc.isObject()) {
        return invalidFrame(QStringLiteral("expected JSON object"));
    }

    const QJsonObject object = doc.object();
    if (const std::optional<QString> shapeError = validate(object);
        shapeError.has_value()) {
        return invalidFrame(shapeError.value());
    }

    DecodeAttempt attempt;
    attempt.status = DecodeStatus::Complete;
    DecodeResult result;
    result.json = object;
    result.bytesConsumed = totalLen;
    attempt.result = result;
    return attempt;
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
