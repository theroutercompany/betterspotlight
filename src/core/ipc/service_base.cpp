#include "core/ipc/service_base.h"
#include "core/shared/logging.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

namespace bs {

namespace {

QString defaultRuntimeRoot()
{
    const uid_t uid = getuid();
    return QStringLiteral("/tmp/betterspotlight-%1").arg(uid);
}

QString normalizedEnvPath(const char* envName)
{
    const QString value = qEnvironmentVariable(envName).trimmed();
    if (value.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(value);
}

} // namespace

ServiceBase::ServiceBase(const QString& serviceName, QObject* parent)
    : QObject(parent)
    , m_serviceName(serviceName)
    , m_server(std::make_unique<SocketServer>(this))
{
    m_server->setRequestHandler([this](const QJsonObject& request) {
        return handleRequest(request);
    });
}

ServiceBase::~ServiceBase() = default;

int ServiceBase::run()
{
    QString path = socketPath(m_serviceName);

    // Ensure the socket directory exists
    QDir dir = QFileInfo(path).dir();
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            qCCritical(bsIpc, "Failed to create socket directory: %s",
                       qPrintable(dir.path()));
            return 1;
        }
    }

    if (!m_server->listen(path)) {
        qCCritical(bsIpc, "Service '%s' failed to start", qPrintable(m_serviceName));
        return 1;
    }

    qCInfo(bsIpc, "Service '%s' started on %s", qPrintable(m_serviceName), qPrintable(path));

    // Signal readiness to supervisor
    fprintf(stdout, "ready\n");
    fflush(stdout);

    return QCoreApplication::exec();
}

QString ServiceBase::socketPath(const QString& serviceName)
{
    return QDir::cleanPath(socketDirectory() + QLatin1Char('/')
                           + serviceName + QStringLiteral(".sock"));
}

QString ServiceBase::runtimeDirectory()
{
    const QString runtimeDir = normalizedEnvPath("BETTERSPOTLIGHT_RUNTIME_DIR");
    if (!runtimeDir.isEmpty()) {
        return runtimeDir;
    }
    return defaultRuntimeRoot();
}

QString ServiceBase::socketDirectory()
{
    const QString socketDir = normalizedEnvPath("BETTERSPOTLIGHT_SOCKET_DIR");
    if (!socketDir.isEmpty()) {
        return socketDir;
    }
    return runtimeDirectory();
}

QString ServiceBase::pidDirectory()
{
    const QString pidDir = normalizedEnvPath("BETTERSPOTLIGHT_PID_DIR");
    if (!pidDir.isEmpty()) {
        return pidDir;
    }
    return runtimeDirectory();
}

QString ServiceBase::pidPath(const QString& serviceName)
{
    return QDir::cleanPath(pidDirectory() + QLatin1Char('/')
                           + serviceName + QStringLiteral(".pid"));
}

QJsonObject ServiceBase::handleRequest(const QJsonObject& request)
{
    QString method = request.value(QStringLiteral("method")).toString();
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());

    if (method == QLatin1String("ping")) {
        return handlePing(request);
    }
    if (method == QLatin1String("shutdown")) {
        return handleShutdown(request);
    }

    qCWarning(bsIpc, "Unknown method '%s' in service '%s'",
              qPrintable(method), qPrintable(m_serviceName));
    return IpcMessage::makeError(id, IpcErrorCode::NotFound,
                                  QStringLiteral("Unknown method: %1").arg(method));
}

QJsonObject ServiceBase::handlePing(const QJsonObject& request)
{
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());

    QJsonObject result;
    result[QStringLiteral("pong")] = true;
    result[QStringLiteral("timestamp")] = QDateTime::currentMSecsSinceEpoch();
    result[QStringLiteral("service")] = m_serviceName;

    qCDebug(bsIpc, "Ping received for service '%s'", qPrintable(m_serviceName));
    return IpcMessage::makeResponse(id, result);
}

QJsonObject ServiceBase::handleShutdown(const QJsonObject& request)
{
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());

    qCInfo(bsIpc, "Shutdown requested for service '%s'", qPrintable(m_serviceName));

    QJsonObject result;
    result[QStringLiteral("shutting_down")] = true;

    // Schedule quit after sending the response
    QMetaObject::invokeMethod(QCoreApplication::instance(), &QCoreApplication::quit,
                              Qt::QueuedConnection);

    return IpcMessage::makeResponse(id, result);
}

void ServiceBase::sendNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject notification = IpcMessage::makeNotification(method, params);
    m_server->broadcast(notification);
}

} // namespace bs
