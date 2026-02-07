#pragma once

#include <QString>
#include <cstdint>

namespace bs {

// IPC error codes (doc 05)
enum class IpcErrorCode : int {
    InvalidParams      = 1,
    Timeout            = 2,
    PermissionDenied   = 3,
    NotFound           = 4,
    AlreadyRunning     = 5,
    InternalError      = 6,
    Unsupported        = 7,
    CorruptedIndex     = 8,
    ServiceUnavailable = 9,
};

QString ipcErrorCodeToString(IpcErrorCode code);

// Generic IPC request envelope
struct IpcRequest {
    uint64_t id = 0;
    QString method;
    QByteArray paramsJson;
};

// Generic IPC response envelope
struct IpcResponse {
    uint64_t id = 0;
    bool isError = false;
    IpcErrorCode errorCode = IpcErrorCode::InternalError;
    QString errorMessage;
    QByteArray resultJson;
};

inline QString ipcErrorCodeToString(IpcErrorCode code)
{
    switch (code) {
    case IpcErrorCode::InvalidParams:      return QStringLiteral("INVALID_PARAMS");
    case IpcErrorCode::Timeout:            return QStringLiteral("TIMEOUT");
    case IpcErrorCode::PermissionDenied:   return QStringLiteral("PERMISSION_DENIED");
    case IpcErrorCode::NotFound:           return QStringLiteral("NOT_FOUND");
    case IpcErrorCode::AlreadyRunning:     return QStringLiteral("ALREADY_RUNNING");
    case IpcErrorCode::InternalError:      return QStringLiteral("INTERNAL_ERROR");
    case IpcErrorCode::Unsupported:        return QStringLiteral("UNSUPPORTED");
    case IpcErrorCode::CorruptedIndex:     return QStringLiteral("CORRUPTED_INDEX");
    case IpcErrorCode::ServiceUnavailable: return QStringLiteral("SERVICE_UNAVAILABLE");
    }
    return QStringLiteral("UNKNOWN");
}

} // namespace bs
