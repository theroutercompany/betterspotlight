#pragma once

#include "core/ipc/socket_client.h"

#include <QJsonObject>
#include <QString>

namespace bs::test {

QString resolveServiceBinary(const QString& binaryName);
bool waitForSocketConnection(SocketClient& client, const QString& socketPath, int timeoutMs);
QJsonObject sendRequestOrEmpty(SocketClient& client,
                               const QString& method,
                               const QJsonObject& params = {},
                               int timeoutMs = 3000);

bool isResponse(const QJsonObject& message);
bool isError(const QJsonObject& message);
QJsonObject resultPayload(const QJsonObject& message);
QJsonObject errorPayload(const QJsonObject& message);

} // namespace bs::test

