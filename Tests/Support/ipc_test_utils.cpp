#include "ipc_test_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTest>

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

bool waitForSocketConnection(SocketClient& client, const QString& socketPath, int timeoutMs)
{
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

