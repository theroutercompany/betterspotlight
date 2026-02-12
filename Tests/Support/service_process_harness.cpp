#include "service_process_harness.h"

#include "ipc_test_utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

    if (!waitForSocketConnection(m_client, m_socketPath, config.connectTimeoutMs)) {
        stop();
        return false;
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
        m_process.waitForFinished(3000);
    }
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }

    if (!m_socketPath.isEmpty()) {
        QFile::remove(m_socketPath);
    }
    m_started = false;
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
    return sendRequestOrEmpty(m_client, method, params, timeoutMs);
}

} // namespace bs::test
