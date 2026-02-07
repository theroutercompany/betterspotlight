#pragma once

#include "core/shared/ipc_messages.h"
#include <QByteArray>
#include <QJsonObject>
#include <optional>

namespace bs {

class IpcMessage {
public:
    // Encode a JSON object to a length-prefixed message (4-byte BE uint32 + UTF-8 JSON)
    static QByteArray encode(const QJsonObject& json);

    // Decode: reads from buffer, returns decoded JSON and bytes consumed.
    // Returns nullopt if buffer doesn't contain a complete message yet.
    struct DecodeResult {
        QJsonObject json;
        int bytesConsumed = 0;
    };
    static std::optional<DecodeResult> decode(const QByteArray& buffer);

    // Helper: build request JSON
    static QJsonObject makeRequest(uint64_t id, const QString& method, const QJsonObject& params = {});

    // Helper: build success response JSON
    static QJsonObject makeResponse(uint64_t id, const QJsonObject& result);

    // Helper: build error response JSON
    static QJsonObject makeError(uint64_t id, IpcErrorCode code, const QString& message);

    // Helper: build notification JSON (no id)
    static QJsonObject makeNotification(const QString& method, const QJsonObject& params = {});

    // Max message size: 16MB
    static constexpr int kMaxMessageSize = 16 * 1024 * 1024;
};

} // namespace bs
