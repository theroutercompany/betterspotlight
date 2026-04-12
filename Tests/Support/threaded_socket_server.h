#pragma once

#include "core/ipc/socket_server.h"

#include <QMetaObject>
#include <QThread>

#include <functional>

namespace bs::test {

class ThreadedSocketServer {
public:
    ThreadedSocketServer() = default;
    ~ThreadedSocketServer()
    {
        stop();
    }

    ThreadedSocketServer(const ThreadedSocketServer&) = delete;
    ThreadedSocketServer& operator=(const ThreadedSocketServer&) = delete;

    bool start(const QString& socketPath,
               std::function<QJsonObject(const QJsonObject&)> handler)
    {
        stop();

        m_server = new bs::SocketServer();
        m_server->setRequestHandler(std::move(handler));
        m_server->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_server, &QObject::deleteLater);
        QObject::connect(&m_thread, &QThread::finished, [this]() {
            m_server = nullptr;
        });
        m_thread.start();

        bool listened = false;
        QMetaObject::invokeMethod(
            m_server,
            [&]() {
                listened = m_server->listen(socketPath);
            },
            Qt::BlockingQueuedConnection);
        return listened;
    }

    void stop()
    {
        if (m_server) {
            QMetaObject::invokeMethod(
                m_server,
                [this]() {
                    if (m_server) {
                        m_server->close();
                    }
                },
                Qt::BlockingQueuedConnection);
        }
        if (m_thread.isRunning()) {
            m_thread.quit();
            m_thread.wait();
        }
        m_server = nullptr;
    }

    void close()
    {
        stop();
    }

private:
    QThread m_thread;
    bs::SocketServer* m_server = nullptr;
};

} // namespace bs::test
