#include "runtime_environment.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace bs {

namespace {

bool ensureDirectory(const QString& path, QString* error)
{
    QDir dir(path);
    if (dir.exists()) {
        return true;
    }
    if (!dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("Failed to create directory: %1").arg(path);
        }
        return false;
    }
    return true;
}

bool writeRuntimeMetadata(const RuntimeContext& context, QString* error)
{
    QJsonObject metadata;
    metadata[QStringLiteral("instance_id")] = context.instanceId;
    metadata[QStringLiteral("app_pid")] =
        static_cast<qint64>(QCoreApplication::applicationPid());
    metadata[QStringLiteral("started_at")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    metadata[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    metadata[QStringLiteral("runtime_dir")] = context.runtimeDir;
    metadata[QStringLiteral("socket_dir")] = context.socketDir;
    metadata[QStringLiteral("pid_dir")] = context.pidDir;

    QFile metadataFile(context.metadataPath);
    if (!metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Failed to write runtime metadata: %1")
                         .arg(context.metadataPath);
        }
        return false;
    }
    metadataFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    metadataFile.close();
    return true;
}

} // namespace

QString runtimeRootPath()
{
    const uid_t uid = getuid();
    return QStringLiteral("/tmp/betterspotlight-%1").arg(uid);
}

QString makeInstanceId()
{
    return QStringLiteral("%1-%2-%3")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QCoreApplication::applicationPid())
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
}

bool processIsAlive(qint64 pid)
{
    if (pid <= 0) {
        return false;
    }
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

void cleanupOrphanRuntimeDirectories(const RuntimeContext& context,
                                    QStringList* removedDirectories)
{
    QDir root(context.runtimeRoot);
    const QFileInfoList entries = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo& entry : entries) {
        if (!entry.isDir()) {
            continue;
        }
        if (entry.absoluteFilePath() == context.runtimeDir) {
            continue;
        }

        const QString instancePath = entry.absoluteFilePath();
        const QString metadataPath =
            QDir(instancePath).filePath(QStringLiteral("instance.json"));
        QFile metadataFile(metadataPath);
        if (!metadataFile.open(QIODevice::ReadOnly)) {
            continue;
        }

        const QJsonDocument metadataDoc =
            QJsonDocument::fromJson(metadataFile.readAll());
        metadataFile.close();
        if (!metadataDoc.isObject()) {
            continue;
        }

        const QJsonObject metadata = metadataDoc.object();
        const qint64 appPid = metadata.value(QStringLiteral("app_pid")).toInteger();
        if (processIsAlive(appPid)) {
            continue;
        }

        QDir staleDir(instancePath);
        if (staleDir.removeRecursively()) {
            if (removedDirectories) {
                removedDirectories->append(instancePath);
            }
        }
    }
}

bool initRuntimeContext(RuntimeContext* context, QString* error)
{
    if (!context) {
        if (error) {
            *error = QStringLiteral("Runtime context output is null");
        }
        return false;
    }

    const QString envRuntimeDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_RUNTIME_DIR").trimmed();
    const QString envSocketDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_SOCKET_DIR").trimmed();
    const QString envPidDir =
        qEnvironmentVariable("BETTERSPOTLIGHT_PID_DIR").trimmed();
    const QString envInstanceId =
        qEnvironmentVariable("BETTERSPOTLIGHT_INSTANCE_ID").trimmed();

    context->runtimeRoot = runtimeRootPath();
    if (!ensureDirectory(context->runtimeRoot, error)) {
        return false;
    }
    context->lockPath =
        QDir(context->runtimeRoot).filePath(QStringLiteral("app.lock"));

    context->instanceId = envInstanceId.isEmpty() ? makeInstanceId() : envInstanceId;
    context->runtimeDir = envRuntimeDir.isEmpty()
        ? QDir(context->runtimeRoot).filePath(context->instanceId)
        : QDir::cleanPath(envRuntimeDir);
    context->socketDir = envSocketDir.isEmpty()
        ? QDir(context->runtimeDir).filePath(QStringLiteral("sockets"))
        : QDir::cleanPath(envSocketDir);
    context->pidDir = envPidDir.isEmpty()
        ? QDir(context->runtimeDir).filePath(QStringLiteral("pids"))
        : QDir::cleanPath(envPidDir);
    context->metadataPath =
        QDir(context->runtimeDir).filePath(QStringLiteral("instance.json"));

    if (!ensureDirectory(context->runtimeDir, error)
        || !ensureDirectory(context->socketDir, error)
        || !ensureDirectory(context->pidDir, error)) {
        return false;
    }

    qputenv("BETTERSPOTLIGHT_INSTANCE_ID", context->instanceId.toUtf8());
    qputenv("BETTERSPOTLIGHT_RUNTIME_DIR", context->runtimeDir.toUtf8());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", context->socketDir.toUtf8());
    qputenv("BETTERSPOTLIGHT_PID_DIR", context->pidDir.toUtf8());
    return writeRuntimeMetadata(*context, error);
}

} // namespace bs
