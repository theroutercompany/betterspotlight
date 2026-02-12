#include "service_process_harness.h"

#include "ipc_test_utils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTest>

#include <algorithm>

namespace {

bool waitForReadyBanner(QProcess& process, int timeoutMs)
{
    if (timeoutMs <= 0) {
        return false;
    }

    const auto channelMode = process.processChannelMode();
    if (channelMode == QProcess::ForwardedChannels
        || channelMode == QProcess::ForwardedOutputChannel
        || channelMode == QProcess::ForwardedErrorChannel) {
        // Cannot capture stdout/stderr in forwarded mode; caller should use ping readiness.
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    QByteArray combinedOutput;

    while (timer.elapsed() < timeoutMs) {
        if (process.state() == QProcess::NotRunning) {
            return false;
        }

        process.waitForReadyRead(50);
        combinedOutput += process.readAllStandardOutput();
        combinedOutput += process.readAllStandardError();

        if (combinedOutput.contains("\nready\n")
            || combinedOutput.endsWith("ready\n")
            || combinedOutput == "ready\n") {
            return true;
        }
    }

    return false;
}

int timeoutForMethod(const QString& method, int fallbackTimeoutMs)
{
    const int defaultTimeoutMs = std::max(500, fallbackTimeoutMs);

    if (method == QLatin1String("startIndexing")
        || method == QLatin1String("rebuildAll")
        || method == QLatin1String("rebuild_vector_index")
        || method == QLatin1String("rebuildVectorIndex")) {
        return std::max(defaultTimeoutMs, 15000);
    }

    if (method == QLatin1String("record_interaction")) {
        return std::max(defaultTimeoutMs, 10000);
    }

    if (method == QLatin1String("embed_passages")) {
        return std::max(defaultTimeoutMs, 8000);
    }

    if (method == QLatin1String("shutdown")) {
        return std::max(defaultTimeoutMs, 3000);
    }

    return defaultTimeoutMs;
}

} // namespace

namespace bs::test {

ServiceProcessHarness::ServiceProcessHarness(QString serviceName, QString binaryName)
    : m_serviceName(std::move(serviceName))
    , m_binaryName(std::move(binaryName))
    , m_socketDir(QStringLiteral("/tmp/bs-svch-XXXXXX"))
{
}

ServiceProcessHarness::~ServiceProcessHarness()
{
    stop();
}

bool ServiceProcessHarness::start(const ServiceLaunchConfig& config)
{
    if (m_started) {
        return true;
    }
    if (!m_socketDir.isValid()) {
        return false;
    }
    m_requestDefaultTimeoutMs = std::max(500, config.requestDefaultTimeoutMs);

    m_binaryPath = resolveServiceBinary(m_binaryName);
    if (m_binaryPath.isEmpty()) {
        return false;
    }

    m_socketPath = ServiceBase::socketPath(m_serviceName);
    m_socketPath = QDir::cleanPath(m_socketDir.path() + QLatin1Char('/') + m_serviceName
                                   + QStringLiteral(".sock"));
    QFile::remove(m_socketPath);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("BETTERSPOTLIGHT_SOCKET_DIR"), m_socketDir.path());
    if (!config.homeDir.isEmpty()) {
        env.insert(QStringLiteral("HOME"), config.homeDir);
        env.insert(QStringLiteral("CFFIXED_USER_HOME"), config.homeDir);
    }
    if (!config.dataDir.isEmpty()) {
        env.insert(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR"), config.dataDir);
        const QFileInfo dataDirInfo(config.dataDir);
        const QString xdgDataHome = dataDirInfo.absolutePath();
        if (!xdgDataHome.isEmpty()) {
            env.insert(QStringLiteral("XDG_DATA_HOME"), xdgDataHome);
        }
    }
    for (auto it = config.env.constBegin(); it != config.env.constEnd(); ++it) {
        env.insert(it.key(), it.value());
    }

    m_process.setProcessEnvironment(env);
    m_process.setProgram(m_binaryPath);
    m_process.setArguments({});
    m_process.setProcessChannelMode(
        config.forwardChannels ? QProcess::ForwardedChannels : QProcess::SeparateChannels);
    m_process.start();
    if (!m_process.waitForStarted(config.startTimeoutMs)) {
        stop();
        return false;
    }

    if (config.waitForReadyBanner
        && !waitForReadyBanner(m_process, std::max(1000, config.readyTimeoutMs))) {
        qWarning() << "Service did not emit ready banner within timeout:" << m_serviceName;
        stop();
        return false;
    }

    if (config.requirePingReady) {
        const int pingTimeoutMs = std::min(std::max(500, m_requestDefaultTimeoutMs), 2000);
        if (!waitForServiceReady(m_client, m_socketPath, config.readyTimeoutMs, pingTimeoutMs)) {
            qWarning() << "Service did not become ping-ready within timeout:" << m_serviceName;
            stop();
            return false;
        }
    } else {
        if (!waitForSocketFile(m_socketPath, config.readyTimeoutMs)
            || !waitForSocketConnection(m_client, m_socketPath, config.connectTimeoutMs)) {
            stop();
            return false;
        }
    }

    m_started = true;
    return true;
}

void ServiceProcessHarness::stop()
{
    if (m_client.isConnected()) {
        m_client.sendRequest(QStringLiteral("shutdown"), {}, 1000);
    }
    m_client.disconnect();

    if (m_process.state() != QProcess::NotRunning) {
        // Give the service time to perform graceful shutdown and flush profile data.
        if (!m_process.waitForFinished(5000)) {
            m_process.terminate();
            if (!m_process.waitForFinished(3000)) {
                m_process.kill();
                m_process.waitForFinished(2000);
            }
        }
    }

    if (!m_socketPath.isEmpty()) {
        QFile::remove(m_socketPath);
    }
    m_started = false;
    m_requestDefaultTimeoutMs = 5000;
}

bool ServiceProcessHarness::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

QString ServiceProcessHarness::socketPath() const
{
    return m_socketPath;
}

QString ServiceProcessHarness::binaryPath() const
{
    return m_binaryPath;
}

SocketClient& ServiceProcessHarness::client()
{
    return m_client;
}

QProcess& ServiceProcessHarness::process()
{
    return m_process;
}

QJsonObject ServiceProcessHarness::request(const QString& method,
                                           const QJsonObject& params,
                                           int timeoutMs)
{
    const int effectiveTimeoutMs = timeoutMs > 0
        ? timeoutMs
        : timeoutForMethod(method, m_requestDefaultTimeoutMs);
    return requestOrFailWithDiagnostics(
        m_client,
        method,
        params,
        effectiveTimeoutMs,
        m_socketPath);
}

} // namespace bs::test
