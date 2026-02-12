#include "ipc_test_utils.h"

#include "core/ipc/message.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTest>

#include <algorithm>

namespace bs::test {

QString resolveServiceBinary(const QString& binaryName)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString serviceFolder = binaryName.startsWith(QStringLiteral("betterspotlight-"))
        ? binaryName.mid(QStringLiteral("betterspotlight-").size())
        : QString();
    const QString serviceRel = serviceFolder.isEmpty() ? QString() : QStringLiteral("services/") + serviceFolder;

    QStringList candidates = {
        QDir(appDir).filePath(binaryName),
        QDir(appDir).filePath(QStringLiteral("../src/") + serviceRel + QStringLiteral("/") + binaryName),
        QDir(appDir).filePath(QStringLiteral("../../src/") + serviceRel + QStringLiteral("/") + binaryName),
        QDir(appDir).filePath(QStringLiteral("../../../src/") + serviceRel + QStringLiteral("/") + binaryName),
        QDir(appDir).filePath(QStringLiteral("../bin/") + binaryName),
        QDir(appDir).filePath(QStringLiteral("../../bin/") + binaryName),
    };

    for (const QString& candidate : candidates) {
        QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) {
            return info.canonicalFilePath();
        }
    }

    return QStandardPaths::findExecutable(binaryName);
}

bool waitForSocketFile(const QString& socketPath, int timeoutMs)
{
    if (socketPath.trimmed().isEmpty() || timeoutMs <= 0) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QFileInfo info(socketPath);
        if (info.exists()) {
            return true;
        }
        QTest::qWait(25);
    }
    return false;
}

bool waitForSocketConnection(SocketClient& client, const QString& socketPath, int timeoutMs)
{
    if (socketPath.trimmed().isEmpty() || timeoutMs <= 0) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (client.connectToServer(socketPath, 100)) {
            return true;
        }
        QTest::qWait(25);
    }
    return false;
}

bool waitForServiceReady(SocketClient& client,
                         const QString& socketPath,
                         int timeoutMs,
                         int pingTimeoutMs)
{
    if (pingTimeoutMs <= 0) {
        pingTimeoutMs = 500;
    }

    QElapsedTimer timer;
    timer.start();

    if (!waitForSocketFile(socketPath, timeoutMs)) {
        return false;
    }

    while (timer.elapsed() < timeoutMs) {
        if (!client.isConnected()) {
            const int remaining = std::max(1, timeoutMs - static_cast<int>(timer.elapsed()));
            const int connectAttemptTimeout = std::min(remaining, 200);
            if (!client.connectToServer(socketPath, connectAttemptTimeout)) {
                QTest::qWait(25);
                continue;
            }
        }

        const int remaining = std::max(1, timeoutMs - static_cast<int>(timer.elapsed()));
        const int pingAttemptTimeout = std::min(pingTimeoutMs, remaining);
        const QJsonObject response = requestOrFailWithDiagnostics(
            client,
            QStringLiteral("ping"),
            {},
            pingAttemptTimeout,
            socketPath);
        if (isResponse(response)
            && resultPayload(response).value(QStringLiteral("pong")).toBool(false)) {
            return true;
        }

        client.disconnect();
        QTest::qWait(25);
    }

    return false;
}

QJsonObject sendRequestOrEmpty(SocketClient& client,
                               const QString& method,
                               const QJsonObject& params,
                               int timeoutMs)
{
    auto response = client.sendRequest(method, params, timeoutMs);
    if (!response.has_value()) {
        return QJsonObject();
    }
    return response.value();
}

QJsonObject requestOrFailWithDiagnostics(SocketClient& client,
                                         const QString& method,
                                         const QJsonObject& params,
                                         int timeoutMs,
                                         const QString& socketPath)
{
    auto response = client.sendRequest(method, params, timeoutMs);
    if (response.has_value()) {
        return response.value();
    }

    QJsonObject diagnostics;
    diagnostics[QStringLiteral("method")] = method;
    diagnostics[QStringLiteral("timeoutMs")] = timeoutMs;
    diagnostics[QStringLiteral("connected")] = client.isConnected();
    if (!socketPath.trimmed().isEmpty()) {
        diagnostics[QStringLiteral("socketPath")] = socketPath;
        diagnostics[QStringLiteral("socketExists")] = QFileInfo::exists(socketPath);
    }

    qWarning().noquote()
        << QStringLiteral("IPC request failed in test harness. Diagnostics=%1")
               .arg(QString::fromUtf8(QJsonDocument(diagnostics).toJson(QJsonDocument::Compact)));

    QJsonObject error = IpcMessage::makeError(
        0,
        IpcErrorCode::Timeout,
        QStringLiteral("Request '%1' failed or timed out after %2ms")
            .arg(method)
            .arg(timeoutMs));
    QJsonObject payload = error.value(QStringLiteral("error")).toObject();
    payload[QStringLiteral("diagnostics")] = diagnostics;
    error[QStringLiteral("error")] = payload;
    return error;
}

bool isResponse(const QJsonObject& message)
{
    return message.value(QStringLiteral("type")).toString() == QLatin1String("response");
}

bool isError(const QJsonObject& message)
{
    return message.value(QStringLiteral("type")).toString() == QLatin1String("error");
}

QJsonObject resultPayload(const QJsonObject& message)
{
    return message.value(QStringLiteral("result")).toObject();
}

QJsonObject errorPayload(const QJsonObject& message)
{
    return message.value(QStringLiteral("error")).toObject();
}

} // namespace bs::test
