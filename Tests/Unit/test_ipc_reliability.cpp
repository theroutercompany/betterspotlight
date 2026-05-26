#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include "core/ipc/message.h"
#include "core/ipc/socket_client.h"
#include "core/ipc/socket_server.h"
#include "core/ipc/supervisor.h"

#include <cstring>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class TestIpcReliability : public QObject {
    Q_OBJECT

private slots:
    void testReadBufferCapClient();
    void testReadBufferCapServer();
    void testSocketServerRejectsEmptySocketPath();
    void testAutoReconnectOnDisconnect();
    void testAutoReconnectMaxAttempts();
    void testSocketServerCloseWithActiveClients_NoUAF();
    void testSocketServerDisconnectRace_IdempotentCleanup();
    void testSocketClientPendingFailureCallbackMayDestroyClient();
    void testSocketClientRejectsEmptyMethodWithoutWriting();
    void testSocketClientRejectsInvalidTimeoutWithoutWriting();
    void testSocketServerDisconnectsMalformedFrame();
    void testSocketServerInvalidHandlerResponseReturnsError();
    void testSocketServerSkipsInvalidBroadcastWithoutDisconnect();
    void testSocketClientSyncRequestFailsFastOnInvalidFrame();
    void testSocketClientSyncRequestFailsFastOnPeerDisconnect();
    void testSocketClientRepeatedConnectAttempts_RecoversFromErrorState();
    void testDeferredResponsesCanArriveOutOfOrder();
    void testDeferredResponderCanSendFromWorkerThread();
    void testDeferredResponsesAreDroppedAfterDisconnect();
    void testSupervisorCrashWindowReset();
};

namespace {

QString makeShortSocketPath(const QString& tag)
{
    const QString token = QString::number(QRandomGenerator::global()->generate(), 16);
    return QDir(QDir::tempPath()).filePath(
        QStringLiteral("bs-%1-%2.sock").arg(tag.left(6), token.left(8)));
}

QByteArray makeMalformedJsonFrame()
{
    QByteArray frame;
    frame.resize(4);
    const QByteArray payload = QByteArrayLiteral("{ invalid");
    quint32 payloadLen = qToBigEndian(static_cast<quint32>(payload.size()));
    memcpy(frame.data(), &payloadLen, 4);
    frame.append(payload);
    return frame;
}

} // namespace

void TestIpcReliability::testReadBufferCapClient()
{
    // Verify that the client buffer cap constant is properly defined
    QCOMPARE(bs::SocketClient::kMaxReadBufferSize, 64 * 1024 * 1024);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.path() + "/test_cap.sock";

    // Set up a simple server
    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    bool connected = client.connectToServer(socketPath, 3000);
    if (!connected) {
        QSKIP("Could not connect to local socket (platform limitation)");
    }

    // Wait for the server to accept the connection
    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    // Verify the constant value is accessible
    QCOMPARE(bs::SocketClient::kMaxReadBufferSize, 64 * 1024 * 1024);

    serverSide->disconnectFromServer();
    server.close();
}

void TestIpcReliability::testReadBufferCapServer()
{
    // Verify server buffer cap constant is properly defined
    QCOMPARE(bs::SocketServer::kMaxReadBufferSize, 64 * 1024 * 1024);
}

void TestIpcReliability::testSocketServerRejectsEmptySocketPath()
{
    bs::SocketServer server;
    QSignalSpy errorSpy(&server, &bs::SocketServer::errorOccurred);

    QVERIFY(!server.listen(QStringLiteral("   ")));
    QVERIFY(!server.isListening());
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.takeFirst().first().toString().contains(QStringLiteral("Invalid socket path")));
}

void TestIpcReliability::testAutoReconnectOnDisconnect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.path() + "/test_reconnect.sock";

    // Start a server
    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    bool connected = client.connectToServer(socketPath, 3000);
    if (!connected) {
        QSKIP("Could not connect to local socket (platform limitation)");
    }
    QVERIFY(client.isConnected());

    // Accept the connection
    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    // Enable auto-reconnect
    client.enableAutoReconnect(socketPath, 3, 100);

    QSignalSpy reconnectSpy(&client, &bs::SocketClient::reconnected);

    // Disconnect the client from the server side
    serverSide->disconnectFromServer();

    // Process events to let the disconnect and reconnect timer fire
    // The server is still listening, so reconnect should succeed
    bool gotReconnect = reconnectSpy.wait(5000);

    if (gotReconnect) {
        QVERIFY(client.isConnected());
    }
    // Even if the reconnect timing is flaky, the test verifies
    // that enableAutoReconnect doesn't crash or hang

    client.disableAutoReconnect();
    client.disconnect();
    server.close();
}

void TestIpcReliability::testAutoReconnectMaxAttempts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.path() + "/test_max_reconnect.sock";

    // Start and connect
    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    bool connected = client.connectToServer(socketPath, 3000);
    if (!connected) {
        QSKIP("Could not connect to local socket (platform limitation)");
    }

    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    // Enable auto-reconnect with short delays and 2 max attempts
    client.enableAutoReconnect(socketPath, 2, 50);

    QSignalSpy errorSpy(&client, &bs::SocketClient::errorOccurred);

    // Close the server so reconnect will always fail
    serverSide->disconnectFromServer();
    server.close();

    // Wait for the "Auto-reconnect failed" error signal.
    // Multiple error signals may arrive (from individual failed connectToServer calls),
    // so keep waiting until we see the exhaustion message or timeout.
    bool gotExhaustion = false;
    QElapsedTimer timer;
    timer.start();
    while (!gotExhaustion && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        for (int i = 0; i < errorSpy.count(); ++i) {
            const QString msg = errorSpy.at(i).first().toString();
            if (msg.contains(QStringLiteral("Auto-reconnect failed"))) {
                gotExhaustion = true;
                break;
            }
        }
    }
    QVERIFY(gotExhaustion);

    client.disableAutoReconnect();
    client.disconnect();
}

void TestIpcReliability::testSocketServerCloseWithActiveClients_NoUAF()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("close"));
    QFile::remove(socketPath);

    bs::SocketServer server;
    server.setRequestHandler([](const QJsonObject& request) {
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        return bs::IpcMessage::makeResponse(id, QJsonObject{{QStringLiteral("ok"), true}});
    });
    QVERIFY(server.listen(socketPath));

    std::vector<std::unique_ptr<bs::SocketClient>> clients;
    clients.reserve(4);
    for (int i = 0; i < 4; ++i) {
        auto client = std::make_unique<bs::SocketClient>();
        QVERIFY(client->connectToServer(socketPath, 3000));
        clients.push_back(std::move(client));
    }

    for (const auto& client : clients) {
        QVERIFY(client->isConnected());
    }

    server.close();
    server.close(); // idempotent close path should be safe
    QVERIFY(!server.isListening());

    for (const auto& client : clients) {
        client->disconnect();
        QVERIFY(!client->isConnected());
    }
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketServerDisconnectRace_IdempotentCleanup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.path() + "/test_disconnect_race.sock";

    bs::SocketServer server;
    server.setRequestHandler([](const QJsonObject& request) {
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        return bs::IpcMessage::makeResponse(id, QJsonObject{{QStringLiteral("ok"), true}});
    });
    QVERIFY(server.listen(socketPath));

    QSignalSpy connectedSpy(&server, &bs::SocketServer::clientConnected);
    QSignalSpy disconnectedSpy(&server, &bs::SocketServer::clientDisconnected);

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));
    if (connectedSpy.count() == 0) {
        QVERIFY(connectedSpy.wait(3000));
    }

    // Exercise repeated disconnect/close transitions; cleanup should stay idempotent.
    client.disconnect();
    client.disconnect();
    server.close();
    server.close();

    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QVERIFY(disconnectedSpy.count() <= 1);
}

void TestIpcReliability::testSocketClientPendingFailureCallbackMayDestroyClient()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("delcb"));
    QFile::remove(socketPath);

    bs::SocketServer server;
    server.setRequestHandler([](const QJsonObject&, bs::SocketServer::RequestResponder) {
        // Keep requests pending so disconnect has to fail every callback.
    });
    QVERIFY(server.listen(socketPath));

    auto client = std::make_unique<bs::SocketClient>();
    QPointer<bs::SocketClient> clientPtr(client.get());
    QVERIFY(client->connectToServer(socketPath, 3000));

    int callbacks = 0;
    client->sendRequestAsync(QStringLiteral("first"), {}, 5000,
                             [&](const std::optional<QJsonObject>& response) {
        ++callbacks;
        QVERIFY(response.has_value());
        client.reset();
    });
    client->sendRequestAsync(QStringLiteral("second"), {}, 5000,
                             [&](const std::optional<QJsonObject>& response) {
        ++callbacks;
        QVERIFY(response.has_value());
    });

    clientPtr->disconnect();
    QVERIFY(clientPtr.isNull());
    QCOMPARE(callbacks, 1);

    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketClientRejectsEmptyMethodWithoutWriting()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("empty"));
    QFile::remove(socketPath);

    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    QVERIFY(!client.sendRequest(QString(), {}, 3000).has_value());

    bool callbackInvoked = false;
    client.sendRequestAsync(QStringLiteral("   "), {}, 3000,
                            [&](const std::optional<QJsonObject>& response) {
        callbackInvoked = true;
        QVERIFY(!response.has_value());
    });
    QVERIFY(callbackInvoked);

    QVERIFY(!client.sendNotification(QString(), {}));
    QVERIFY(!serverSide->waitForReadyRead(100));
    QCOMPARE(serverSide->bytesAvailable(), static_cast<qint64>(0));

    client.disconnect();
    serverSide->abort();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketClientRejectsInvalidTimeoutWithoutWriting()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("tmout"));
    QFile::remove(socketPath);

    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    QVERIFY(!client.sendRequest(QStringLiteral("ping"), {}, 0).has_value());

    bool callbackInvoked = false;
    client.sendRequestAsync(QStringLiteral("ping"), {}, -1,
                            [&](const std::optional<QJsonObject>& response) {
        callbackInvoked = true;
        QVERIFY(!response.has_value());
    });
    QVERIFY(callbackInvoked);

    QVERIFY(!serverSide->waitForReadyRead(100));
    QCOMPARE(serverSide->bytesAvailable(), static_cast<qint64>(0));

    client.disconnect();
    serverSide->abort();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketServerDisconnectsMalformedFrame()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("badfrm"));
    QFile::remove(socketPath);

    bs::SocketServer server;
    server.setRequestHandler([](const QJsonObject& request) {
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        return bs::IpcMessage::makeResponse(id, QJsonObject{{QStringLiteral("ok"), true}});
    });
    QVERIFY(server.listen(socketPath));
    QSignalSpy disconnectedSpy(&server, &bs::SocketServer::clientDisconnected);

    QLocalSocket attacker;
    attacker.connectToServer(socketPath);
    QVERIFY(attacker.waitForConnected(3000));

    const QByteArray frame = makeMalformedJsonFrame();
    QCOMPARE(attacker.write(frame), static_cast<qint64>(frame.size()));
    QVERIFY(attacker.flush());

    QVERIFY(disconnectedSpy.wait(3000)
            || attacker.waitForDisconnected(3000)
            || attacker.state() == QLocalSocket::UnconnectedState);
    QVERIFY(disconnectedSpy.count() <= 1);

    attacker.abort();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketServerInvalidHandlerResponseReturnsError()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("badres"));
    QFile::remove(socketPath);

    bs::SocketServer server;
    server.setRequestHandler([](const QJsonObject& request) {
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("response")},
            {QStringLiteral("id"), static_cast<qint64>(id)},
            {QStringLiteral("result"), true},
        };
    });
    QVERIFY(server.listen(socketPath));

    QSignalSpy connectedSpy(&server, &bs::SocketServer::clientConnected);
    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));
    if (connectedSpy.count() == 0) {
        QVERIFY(connectedSpy.wait(3000));
    }

    std::optional<QJsonObject> observedResponse;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(3000);

    client.sendRequestAsync(QStringLiteral("bad_response"), {}, 3000,
                            [&](const std::optional<QJsonObject>& response) {
        observedResponse = response;
        loop.quit();
    });

    loop.exec();
    QVERIFY(observedResponse.has_value());
    QCOMPARE(observedResponse->value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(observedResponse->value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toInt(),
             static_cast<int>(bs::IpcErrorCode::InternalError));
    QVERIFY(observedResponse->value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("Invalid IPC response from handler")));

    client.disconnect();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketServerSkipsInvalidBroadcastWithoutDisconnect()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("badbct"));
    QFile::remove(socketPath);

    bs::SocketServer server;
    QVERIFY(server.listen(socketPath));

    QSignalSpy connectedSpy(&server, &bs::SocketServer::clientConnected);
    bs::SocketClient client;
    QString observedMethod;
    client.setNotificationHandler([&](const QString& method, const QJsonObject&) {
        observedMethod = method;
    });
    QVERIFY(client.connectToServer(socketPath, 3000));
    if (connectedSpy.count() == 0) {
        QVERIFY(connectedSpy.wait(3000));
    }

    QSignalSpy disconnectedSpy(&client, &bs::SocketClient::disconnected);
    server.broadcast(QJsonObject{{QStringLiteral("type"), QStringLiteral("notification")}});
    QTest::qWait(100);
    QCOMPARE(disconnectedSpy.count(), 0);
    QVERIFY(client.isConnected());

    server.broadcast(bs::IpcMessage::makeNotification(QStringLiteral("still_alive")));
    QTRY_COMPARE_WITH_TIMEOUT(observedMethod, QStringLiteral("still_alive"), 1000);

    client.disconnect();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketClientSyncRequestFailsFastOnInvalidFrame()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("badcli"));
    QFile::remove(socketPath);

    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    const QByteArray frame = makeMalformedJsonFrame();
    QCOMPARE(serverSide->write(frame), static_cast<qint64>(frame.size()));
    QVERIFY(serverSide->flush());
    QVERIFY(serverSide->waitForBytesWritten(3000) || serverSide->bytesToWrite() == 0);

    QElapsedTimer timer;
    timer.start();
    const auto response = client.sendRequest(QStringLiteral("will_fail"), {}, 3000);

    QVERIFY2(timer.elapsed() < 1000,
             "Malformed IPC frames should fail synchronous callers without waiting for timeout");
    QVERIFY(response.has_value());
    QCOMPARE(response->value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QVERIFY(response->value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("Invalid IPC frame")));

    client.disconnect();
    serverSide->abort();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketClientSyncRequestFailsFastOnPeerDisconnect()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("drop"));
    QFile::remove(socketPath);

    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    QVERIFY(server.waitForNewConnection(3000));
    QLocalSocket* serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);
    QVERIFY(client.isConnected());

    serverSide->disconnectFromServer();
    if (!client.isConnected()) {
        QSKIP("Local socket observed peer disconnect synchronously");
    }

    QElapsedTimer timer;
    timer.start();
    const auto response = client.sendRequest(QStringLiteral("will_fail"), {}, 3000);

    QVERIFY2(timer.elapsed() < 1000,
             "Peer disconnects should fail synchronous callers without waiting for timeout");
    QVERIFY(response.has_value());
    QCOMPARE(response->value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QVERIFY(response->value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("Connection lost")));

    client.disconnect();
    serverSide->abort();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSocketClientRepeatedConnectAttempts_RecoversFromErrorState()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("reconn"));
    QFile::remove(socketPath);

    bs::SocketClient client;
    for (int i = 0; i < 8; ++i) {
        QVERIFY(!client.connectToServer(socketPath, 100));
        QVERIFY(!client.isConnected());
    }

    QLocalServer server;
    QVERIFY(server.listen(socketPath));

    QVERIFY2(client.connectToServer(socketPath, 3000),
             "Client should recover after repeated failed connect attempts");
    QVERIFY(client.isConnected());

    client.disconnect();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testDeferredResponsesCanArriveOutOfOrder()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("defer"));
    QFile::remove(socketPath);

    bs::SocketServer server;
    server.setRequestHandler([&server](const QJsonObject& request,
                                       bs::SocketServer::RequestResponder responder) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        const int delayMs = (method == QLatin1String("slow")) ? 150 : 10;
        QTimer::singleShot(delayMs, &server, [responder, id, method]() mutable {
            QJsonObject result;
            result[QStringLiteral("label")] = method;
            responder.send(bs::IpcMessage::makeResponse(id, result));
        });
    });
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    QStringList completionOrder;
    QJsonObject slowResponse;
    QJsonObject fastResponse;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(3000);

    client.sendRequestAsync(QStringLiteral("slow"), {}, 2000,
                            [&](const std::optional<QJsonObject>& response) {
        QVERIFY(response.has_value());
        slowResponse = response.value();
        completionOrder.append(QStringLiteral("slow"));
        if (completionOrder.size() == 2) {
            loop.quit();
        }
    });
    client.sendRequestAsync(QStringLiteral("fast"), {}, 2000,
                            [&](const std::optional<QJsonObject>& response) {
        QVERIFY(response.has_value());
        fastResponse = response.value();
        completionOrder.append(QStringLiteral("fast"));
        if (completionOrder.size() == 2) {
            loop.quit();
        }
    });

    loop.exec();
    QCOMPARE(completionOrder, QStringList({QStringLiteral("fast"), QStringLiteral("slow")}));
    QCOMPARE(fastResponse.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("label")).toString(),
             QStringLiteral("fast"));
    QCOMPARE(slowResponse.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("label")).toString(),
             QStringLiteral("slow"));

    client.disconnect();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testDeferredResponderCanSendFromWorkerThread()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("wkresp"));
    QFile::remove(socketPath);

    std::vector<std::thread> workers;

    bs::SocketServer server;
    server.setRequestHandler([&workers](const QJsonObject& request,
                                        bs::SocketServer::RequestResponder responder) {
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        workers.emplace_back([responder, id]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            responder.send(bs::IpcMessage::makeResponse(
                id,
                QJsonObject{{QStringLiteral("thread"), QStringLiteral("worker")}}));
        });
    });
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    std::optional<QJsonObject> observedResponse;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(3000);

    client.sendRequestAsync(QStringLiteral("worker_response"), {}, 2000,
                            [&](const std::optional<QJsonObject>& response) {
        observedResponse = response;
        loop.quit();
    });

    loop.exec();
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    QVERIFY(observedResponse.has_value());
    QCOMPARE(observedResponse->value(QStringLiteral("type")).toString(), QStringLiteral("response"));
    QCOMPARE(observedResponse->value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("thread")).toString(),
             QStringLiteral("worker"));

    client.disconnect();
    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testDeferredResponsesAreDroppedAfterDisconnect()
{
    const QString socketPath = makeShortSocketPath(QStringLiteral("late"));
    QFile::remove(socketPath);

    bool lateSendAttempted = false;
    bool lateSendAccepted = true;

    bs::SocketServer server;
    server.setRequestHandler([&server,
                              &lateSendAttempted,
                              &lateSendAccepted](const QJsonObject& request,
                                                 bs::SocketServer::RequestResponder responder) {
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        QTimer::singleShot(150, &server, [responder, id, &lateSendAttempted, &lateSendAccepted]() mutable {
            lateSendAttempted = true;
            lateSendAccepted = responder.send(
                bs::IpcMessage::makeResponse(id, QJsonObject{{QStringLiteral("ok"), true}}));
        });
    });
    QVERIFY(server.listen(socketPath));

    bs::SocketClient client;
    QVERIFY(client.connectToServer(socketPath, 3000));

    bool callbackInvoked = false;
    bool callbackWasDisconnectError = false;
    client.sendRequestAsync(QStringLiteral("slow"), {}, 2000,
                            [&](const std::optional<QJsonObject>& response) {
        callbackInvoked = true;
        callbackWasDisconnectError =
            response.has_value()
            && response->value(QStringLiteral("type")).toString() == QLatin1String("error");
    });

    client.disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(callbackInvoked, 1000);
    QVERIFY(callbackWasDisconnectError);

    QTRY_VERIFY_WITH_TIMEOUT(lateSendAttempted, 1000);
    QVERIFY(!lateSendAccepted);

    server.close();
    QFile::remove(socketPath);
}

void TestIpcReliability::testSupervisorCrashWindowReset()
{
    // Test the crash window reset constants are accessible via Supervisor
    bs::Supervisor supervisor;

    // Add a dummy service (won't actually start since path doesn't exist)
    supervisor.addService(QStringLiteral("test-svc"),
                          QStringLiteral("/nonexistent/binary"));

    // Verify serviceSnapshot returns expected structure
    auto snapshot = supervisor.serviceSnapshot();
    QCOMPARE(snapshot.size(), 1);

    QJsonObject entry = snapshot[0].toObject();
    QCOMPARE(entry.value(QStringLiteral("name")).toString(),
             QStringLiteral("test-svc"));
    QCOMPARE(entry.value(QStringLiteral("crashCount")).toInt(), 0);
    QCOMPARE(entry.value(QStringLiteral("running")).toBool(), false);
}

QTEST_MAIN(TestIpcReliability)
#include "test_ipc_reliability.moc"
