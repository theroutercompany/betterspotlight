#pragma once

#include "core/ipc/socket_client.h"

#include <QJsonObject>
#include <QString>

namespace bs::test {

QString resolveServiceBinary(const QString& binaryName);
bool waitForSocketFile(const QString& socketPath, int timeoutMs);
bool waitForSocketConnection(SocketClient& client, const QString& socketPath, int timeoutMs);
bool waitForServiceReady(SocketClient& client,
                         const QString& socketPath,
                         int timeoutMs,
                         int pingTimeoutMs);
QJsonObject sendRequestOrEmpty(SocketClient& client,
                               const QString& method,
                               const QJsonObject& params = {},
                               int timeoutMs = 3000);
QJsonObject requestOrFailWithDiagnostics(SocketClient& client,
                                         const QString& method,
                                         const QJsonObject& params = {},
                                         int timeoutMs = 3000,
                                         const QString& socketPath = {});

bool isResponse(const QJsonObject& message);
bool isError(const QJsonObject& message);
QJsonObject resultPayload(const QJsonObject& message);
QJsonObject errorPayload(const QJsonObject& message);

} // namespace bs::test
