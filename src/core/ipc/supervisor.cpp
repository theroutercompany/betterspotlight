#include "core/ipc/supervisor.h"
#include "core/ipc/service_base.h"
#include "core/shared/logging.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonObject>

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

namespace bs {

Supervisor::Supervisor(QObject* parent)
    : QObject(parent)
    , m_heartbeatTimer(std::make_unique<QTimer>(this))
{
    connect(m_heartbeatTimer.get(), &QTimer::timeout,
            this, &Supervisor::heartbeat);
}

Supervisor::~Supervisor()
{
    stopAll();
}

void Supervisor::addService(const QString& name, const QString& execPath)
{
    auto svc = std::make_unique<ManagedService>();
    svc->info.name = name;
    svc->info.executablePath = execPath;
    svc->info.crashCount = 0;
    svc->info.lastCrashTime = 0;
    svc->ready = false;

    qCInfo(bsIpc, "Registered service '%s' -> %s",
           qPrintable(name), qPrintable(execPath));

    m_services.push_back(std::move(svc));
}

bool Supervisor::startAll()
{
    createSocketDirectory();

    bool allStarted = true;
    for (auto& svc : m_services) {
        startService(*svc);
        if (!svc->process || svc->process->state() == QProcess::NotRunning) {
            allStarted = false;
        }
    }

    // Start the heartbeat timer
    m_heartbeatTimer->start(kHeartbeatIntervalMs);

    return allStarted;
}

void Supervisor::stopAll()
{
    m_heartbeatTimer->stop();

    for (auto& svc : m_services) {
        if (!svc->process) continue;

        qCInfo(bsIpc, "Stopping service '%s'", qPrintable(svc->info.name));

        // Try graceful shutdown via IPC first
        if (svc->client && svc->client->isConnected()) {
            svc->client->sendRequest(QStringLiteral("shutdown"), {}, 2000);
            svc->client->disconnect();
        }

        if (svc->process->state() != QProcess::NotRunning) {
            // Wait briefly for graceful exit
            if (!svc->process->waitForFinished(5000)) {
                qCWarning(bsIpc, "Service '%s' did not exit gracefully, terminating",
                          qPrintable(svc->info.name));
                svc->process->terminate();
                if (!svc->process->waitForFinished(2000)) {
                    qCWarning(bsIpc, "Service '%s' did not respond to SIGTERM, killing",
                              qPrintable(svc->info.name));
                    svc->process->kill();
                    svc->process->waitForFinished(1000);
                }
            }
        }

        svc->ready = false;

        // Remove PID file
        QString pidPath = QStringLiteral("/tmp/betterspotlight-%1/%2.pid")
                              .arg(getuid())
                              .arg(svc->info.name);
        QFile::remove(pidPath);

        emit serviceStopped(svc->info.name);
    }
}

SocketClient* Supervisor::clientFor(const QString& serviceName)
{
    ManagedService* svc = findService(serviceName);
    if (!svc) {
        qCWarning(bsIpc, "No service registered with name '%s'", qPrintable(serviceName));
        return nullptr;
    }
    return svc->client.get();
}

void Supervisor::onServiceFinished(int exitCode, QProcess::ExitStatus status)
{
    auto* process = qobject_cast<QProcess*>(sender());
    if (!process) return;

    // Find which service this process belongs to
    ManagedService* svc = nullptr;
    for (auto& s : m_services) {
        if (s->process.get() == process) {
            svc = s.get();
            break;
        }
    }
    if (!svc) return;

    svc->ready = false;

    if (status == QProcess::CrashExit || exitCode != 0) {
        int64_t now = QDateTime::currentSecsSinceEpoch();

        if (svc->info.crashCount == 0 || now - svc->info.firstCrashTime > kCrashWindowSeconds) {
            svc->info.crashCount = 0;
            svc->info.firstCrashTime = now;
        }

        svc->info.crashCount++;
        svc->info.lastCrashTime = now;

        qCWarning(bsIpc, "Service '%s' crashed (exit=%d, crashes=%d/%d in window)",
                  qPrintable(svc->info.name), exitCode,
                  svc->info.crashCount, kMaxCrashesBeforeGiveUp);

        emit serviceCrashed(svc->info.name, svc->info.crashCount);

        if (svc->info.crashCount >= kMaxCrashesBeforeGiveUp) {
            qCCritical(bsIpc, "Service '%s' crashed %d times in %ds, giving up",
                       qPrintable(svc->info.name),
                       svc->info.crashCount, kCrashWindowSeconds);
            return;
        }

        // Schedule restart with backoff
        int delay = restartDelayMs(svc->info.crashCount);
        qCInfo(bsIpc, "Restarting service '%s' in %dms",
               qPrintable(svc->info.name), delay);

        QTimer::singleShot(delay, this, [this, name = svc->info.name]() {
            ManagedService* s = findService(name);
            if (s) {
                restartService(*s);
            }
        });
    } else {
        qCInfo(bsIpc, "Service '%s' exited normally (code=%d)",
               qPrintable(svc->info.name), exitCode);
        emit serviceStopped(svc->info.name);
    }
}

void Supervisor::heartbeat()
{
    bool allReady = true;
    bool anyChanged = false;

    for (auto& svc : m_services) {
        if (!svc->process || svc->process->state() != QProcess::Running) {
            allReady = false;
            continue;
        }

        // Try to connect client if not yet connected
        if (!svc->client) {
            svc->client = std::make_unique<SocketClient>(this);
        }

        if (!svc->client->isConnected()) {
            QString path = ServiceBase::socketPath(svc->info.name);
            if (svc->client->connectToServer(path, 1000)) {
                qCInfo(bsIpc, "Connected to service '%s'", qPrintable(svc->info.name));
            } else {
                allReady = false;
                continue;
            }
        }

        // Send ping with 5s timeout
        auto response = svc->client->sendRequest(QStringLiteral("ping"), {}, 5000);
        if (!response) {
            qCWarning(bsIpc, "Heartbeat failed for service '%s'", qPrintable(svc->info.name));

            if (svc->ready) {
                svc->ready = false;
                anyChanged = true;
            }
            allReady = false;
            continue;
        }

        // Check for error response
        QString type = response->value(QStringLiteral("type")).toString();
        if (type == QLatin1String("error")) {
            qCWarning(bsIpc, "Heartbeat error for service '%s'", qPrintable(svc->info.name));
            if (svc->ready) {
                svc->ready = false;
                anyChanged = true;
            }
            allReady = false;
            continue;
        }

        if (!svc->ready) {
            svc->ready = true;
            anyChanged = true;
            qCInfo(bsIpc, "Service '%s' is ready", qPrintable(svc->info.name));
            emit serviceStarted(svc->info.name);
        }
    }

    if (allReady && anyChanged && !m_services.empty()) {
        qCInfo(bsIpc, "All services ready");
        emit allServicesReady();
    }
}

void Supervisor::startService(ManagedService& svc)
{
    qCInfo(bsIpc, "Starting service '%s': %s",
           qPrintable(svc.info.name), qPrintable(svc.info.executablePath));

    svc.process = std::make_unique<QProcess>(this);
    svc.process->setProgram(svc.info.executablePath);

    // Forward service stdout/stderr to the parent process
    svc.process->setProcessChannelMode(QProcess::ForwardedChannels);

    connect(svc.process.get(),
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Supervisor::onServiceFinished);

    svc.process->start();

    if (!svc.process->waitForStarted(5000)) {
        qCCritical(bsIpc, "Failed to start service '%s': %s",
                   qPrintable(svc.info.name),
                   qPrintable(svc.process->errorString()));
        svc.process.reset();
        return;
    }

    qCInfo(bsIpc, "Service '%s' started (pid=%lld)",
           qPrintable(svc.info.name), svc.process->processId());

    // Write PID file so external tools can identify our child processes
    QString pidPath = QStringLiteral("/tmp/betterspotlight-%1/%2.pid")
                          .arg(getuid())
                          .arg(svc.info.name);
    QFile pidFile(pidPath);
    if (pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        pidFile.write(QByteArray::number(static_cast<qint64>(svc.process->processId())));
        pidFile.close();
    } else {
        qCWarning(bsIpc, "Failed to write PID file: %s", qPrintable(pidPath));
    }

    // Create client and attempt initial connection after a short delay.
    // The service needs a moment to set up its socket.
    svc.client = std::make_unique<SocketClient>(this);
    QTimer::singleShot(500, this, [this, name = svc.info.name]() {
        ManagedService* s = findService(name);
        if (!s || !s->client) return;

        QString path = ServiceBase::socketPath(name);
        if (s->client->connectToServer(path, 3000)) {
            s->ready = true;
            qCInfo(bsIpc, "Initial connection to service '%s' succeeded", qPrintable(name));
            emit serviceStarted(name);

            // Check if all services are now ready
            bool everyoneReady = true;
            for (const auto& managed : m_services) {
                if (!managed->ready) {
                    everyoneReady = false;
                    break;
                }
            }
            if (everyoneReady && !m_services.empty()) {
                emit allServicesReady();
            }
        }
    });
}

void Supervisor::restartService(ManagedService& svc)
{
    qCInfo(bsIpc, "Restarting service '%s'", qPrintable(svc.info.name));

    // Clean up existing client
    if (svc.client) {
        svc.client->disconnect();
        svc.client.reset();
    }

    // Clean up existing process
    if (svc.process) {
        svc.process->disconnect(this);
        if (svc.process->state() != QProcess::NotRunning) {
            svc.process->kill();
            svc.process->waitForFinished(1000);
        }
        svc.process.reset();
    }

    svc.ready = false;

    // Remove stale PID file before restarting
    QString pidPath = QStringLiteral("/tmp/betterspotlight-%1/%2.pid")
                          .arg(getuid())
                          .arg(svc.info.name);
    QFile::remove(pidPath);

    startService(svc);
}

int Supervisor::restartDelayMs(int crashCount) const
{
    // Exponential backoff: 0ms, 1000ms, 2000ms, 4000ms, ... capped at 30000ms
    if (crashCount <= 1) return 0;

    int delay = 1000 * (1 << (crashCount - 2));  // 1s, 2s, 4s, 8s, ...
    return std::min(delay, 30000);
}

void Supervisor::createSocketDirectory()
{
    uid_t uid = getuid();
    QString dirPath = QStringLiteral("/tmp/betterspotlight-%1").arg(uid);

    QDir dir(dirPath);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            qCCritical(bsIpc, "Failed to create socket directory: %s",
                       qPrintable(dirPath));
            return;
        }
        qCInfo(bsIpc, "Created socket directory: %s", qPrintable(dirPath));
    }
}

Supervisor::ManagedService* Supervisor::findService(const QString& name)
{
    for (auto& svc : m_services) {
        if (svc->info.name == name) {
            return svc.get();
        }
    }
    return nullptr;
}

} // namespace bs
