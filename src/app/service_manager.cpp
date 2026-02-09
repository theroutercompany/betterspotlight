#include "service_manager.h"
#include "core/shared/logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>

namespace bs {

namespace {

QJsonArray defaultCuratedRoots()
{
    const QString home = QDir::homePath();
    return QJsonArray{
        home + QStringLiteral("/Documents"),
        home + QStringLiteral("/Desktop"),
        home + QStringLiteral("/Downloads"),
    };
}

QJsonArray rootsFromIndexRoots(const QJsonObject& settings, bool embedOnly)
{
    QJsonArray roots;
    const QJsonArray indexRoots = settings.value(QStringLiteral("indexRoots")).toArray();
    for (const QJsonValue& value : indexRoots) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        const QString mode = obj.value(QStringLiteral("mode")).toString();
        if (mode == QLatin1String("skip")) {
            continue;
        }
        if (embedOnly && mode != QLatin1String("index_embed")) {
            continue;
        }
        const QString path = obj.value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            roots.append(path);
        }
    }
    return roots;
}

QJsonArray rootsFromHomeDirectories(const QJsonObject& settings, bool embedOnly)
{
    QJsonArray roots;
    const QString home = QDir::homePath();
    const QJsonArray homeDirectories =
        settings.value(QStringLiteral("home_directories")).toArray();
    for (const QJsonValue& value : homeDirectories) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject obj = value.toObject();
        const QString mode = obj.value(QStringLiteral("mode")).toString();
        if (mode == QLatin1String("skip")) {
            continue;
        }
        if (embedOnly && mode != QLatin1String("index_embed")) {
            continue;
        }

        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }

        roots.append(home + QLatin1Char('/') + name);
    }
    return roots;
}

bool isSingleHomeRoot(const QJsonArray& roots)
{
    return roots.size() == 1 && roots.first().toString() == QDir::homePath();
}

QJsonObject readAppSettings()
{
    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/settings.json");
    QFile settingsFile(settingsPath);
    if (!settingsFile.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(settingsFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    return doc.object();
}

} // namespace

ServiceManager::ServiceManager(QObject* parent)
    : QObject(parent)
    , m_supervisor(std::make_unique<Supervisor>(this))
    , m_indexerStatus(QStringLiteral("stopped"))
    , m_extractorStatus(QStringLiteral("stopped"))
    , m_queryStatus(QStringLiteral("stopped"))
{
    connect(m_supervisor.get(), &Supervisor::serviceStarted,
            this, &ServiceManager::onServiceStarted);
    connect(m_supervisor.get(), &Supervisor::serviceStopped,
            this, &ServiceManager::onServiceStopped);
    connect(m_supervisor.get(), &Supervisor::serviceCrashed,
            this, &ServiceManager::onServiceCrashed);
    connect(m_supervisor.get(), &Supervisor::allServicesReady,
            this, &ServiceManager::onAllServicesReady);
}

ServiceManager::~ServiceManager()
{
    stop();
}

bool ServiceManager::isReady() const
{
    return m_allReady;
}

QString ServiceManager::indexerStatus() const
{
    return m_indexerStatus;
}

QString ServiceManager::extractorStatus() const
{
    return m_extractorStatus;
}

QString ServiceManager::queryStatus() const
{
    return m_queryStatus;
}

Supervisor* ServiceManager::supervisor() const
{
    return m_supervisor.get();
}

void ServiceManager::start()
{
    LOG_INFO(bsCore, "ServiceManager: starting services");

    // Register all three service binaries
    const QStringList serviceNames = {
        QStringLiteral("indexer"),
        QStringLiteral("extractor"),
        QStringLiteral("query"),
    };

    for (const auto& name : serviceNames) {
        QString binary = findServiceBinary(name);
        if (binary.isEmpty()) {
            LOG_ERROR(bsCore, "ServiceManager: could not find binary for service '%s'",
                      qPrintable(name));
            emit serviceError(name, QStringLiteral("Binary not found"));
            updateServiceStatus(name, QStringLiteral("error"));
            continue;
        }

        LOG_INFO(bsCore, "ServiceManager: registering service '%s' -> %s",
                 qPrintable(name), qPrintable(binary));
        m_supervisor->addService(name, binary);
        updateServiceStatus(name, QStringLiteral("starting"));
    }

    if (!m_supervisor->startAll()) {
        LOG_WARN(bsCore, "ServiceManager: not all services started cleanly");
    }
}

void ServiceManager::stop()
{
    LOG_INFO(bsCore, "ServiceManager: stopping services");
    m_supervisor->stopAll();

    m_allReady = false;
    m_indexerStatus = QStringLiteral("stopped");
    m_extractorStatus = QStringLiteral("stopped");
    m_queryStatus = QStringLiteral("stopped");
    emit serviceStatusChanged();
}

void ServiceManager::onServiceStarted(const QString& name)
{
    LOG_INFO(bsCore, "ServiceManager: service '%s' started", qPrintable(name));
    updateServiceStatus(name, QStringLiteral("running"));
}

void ServiceManager::onServiceStopped(const QString& name)
{
    LOG_INFO(bsCore, "ServiceManager: service '%s' stopped", qPrintable(name));
    m_allReady = false;
    updateServiceStatus(name, QStringLiteral("stopped"));
}

void ServiceManager::onServiceCrashed(const QString& name, int crashCount)
{
    LOG_WARN(bsCore, "ServiceManager: service '%s' crashed (count=%d)",
             qPrintable(name), crashCount);
    m_allReady = false;
    updateServiceStatus(name, QStringLiteral("crashed"));
    emit serviceError(name,
                      QStringLiteral("Service crashed (%1 times)").arg(crashCount));
}

void ServiceManager::onAllServicesReady()
{
    LOG_INFO(bsCore, "ServiceManager: all services ready");
    m_allReady = true;
    emit serviceStatusChanged();
    emit allServicesReady();

    // Auto-start indexing with default roots
    startIndexing();
}

void ServiceManager::startIndexing()
{
    QJsonObject params;
    params[QStringLiteral("roots")] = loadIndexRoots();

    LOG_INFO(bsCore, "ServiceManager: sending startIndexing (%d root(s))",
             static_cast<int>(params.value(QStringLiteral("roots")).toArray().size()));
    sendIndexerRequest(QStringLiteral("startIndexing"), params);
}

void ServiceManager::pauseIndexing()
{
    sendIndexerRequest(QStringLiteral("pauseIndexing"));
}

void ServiceManager::resumeIndexing()
{
    sendIndexerRequest(QStringLiteral("resumeIndexing"));
}

void ServiceManager::setIndexingUserActive(bool active)
{
    QJsonObject params;
    params[QStringLiteral("active")] = active;
    sendIndexerRequest(QStringLiteral("setUserActive"), params);
}

void ServiceManager::rebuildAll()
{
    sendIndexerRequest(QStringLiteral("rebuildAll"));
}

void ServiceManager::rebuildVectorIndex()
{
    SocketClient* client = m_supervisor->clientFor(QStringLiteral("query"));
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "ServiceManager: query not connected, can't send 'rebuildVectorIndex'");
        return;
    }

    QJsonObject params;
    const QJsonArray embedRoots = loadEmbeddingRoots();
    if (!embedRoots.isEmpty()) {
        params[QStringLiteral("includePaths")] = embedRoots;
    }

    auto response = client->sendRequest(QStringLiteral("rebuildVectorIndex"), params, 10000);
    if (!response) {
        LOG_ERROR(bsCore, "ServiceManager: query request failed: rebuildVectorIndex");
        return;
    }

    const QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        const QString msg = response->value(QStringLiteral("error")).toObject()
                                .value(QStringLiteral("message")).toString();
        LOG_ERROR(bsCore, "ServiceManager: query 'rebuildVectorIndex' error: %s",
                  qPrintable(msg));
        emit serviceError(QStringLiteral("query"), msg);
        return;
    }

    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    const bool started = result.value(QStringLiteral("started")).toBool();
    const bool alreadyRunning = result.value(QStringLiteral("alreadyRunning")).toBool();
    const qint64 runId = result.value(QStringLiteral("runId")).toInteger();
    if (alreadyRunning) {
        LOG_INFO(bsCore, "ServiceManager: vector rebuild already running (runId=%lld)",
                 static_cast<long long>(runId));
    } else if (started) {
        LOG_INFO(bsCore, "ServiceManager: vector rebuild started (runId=%lld)",
                 static_cast<long long>(runId));
    }
}

void ServiceManager::clearExtractionCache()
{
    sendServiceRequest(QStringLiteral("extractor"), QStringLiteral("clearExtractionCache"));
}

void ServiceManager::reindexPath(const QString& path)
{
    QString normalizedPath = path;
    if (normalizedPath.startsWith(QStringLiteral("file://"))) {
        normalizedPath = QUrl(normalizedPath).toLocalFile();
    }
    QJsonObject params;
    params[QStringLiteral("path")] = normalizedPath;
    sendIndexerRequest(QStringLiteral("reindexPath"), params);
}

QVariantList ServiceManager::serviceDiagnostics() const
{
    if (!m_supervisor) {
        return {};
    }
    return m_supervisor->serviceSnapshot().toVariantList();
}

bool ServiceManager::sendIndexerRequest(const QString& method, const QJsonObject& params)
{
    return sendServiceRequest(QStringLiteral("indexer"), method, params);
}

bool ServiceManager::sendServiceRequest(const QString& serviceName,
                                        const QString& method,
                                        const QJsonObject& params)
{
    SocketClient* client = m_supervisor->clientFor(serviceName);
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "ServiceManager: %s not connected, can't send '%s'",
                 qPrintable(serviceName), qPrintable(method));
        return false;
    }

    auto response = client->sendRequest(method, params, 10000);
    if (!response) {
        LOG_ERROR(bsCore, "ServiceManager: %s request failed: %s",
                  qPrintable(serviceName), qPrintable(method));
        return false;
    }

    QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        QString msg = response->value(QStringLiteral("error")).toObject()
                          .value(QStringLiteral("message")).toString();
        LOG_ERROR(bsCore, "ServiceManager: %s '%s' error: %s",
                  qPrintable(serviceName), qPrintable(method), qPrintable(msg));
        emit serviceError(serviceName, msg);
        return false;
    }

    return true;
}

QJsonArray ServiceManager::loadIndexRoots() const
{
    const QJsonObject settings = readAppSettings();

    QJsonArray roots = rootsFromIndexRoots(settings, /*embedOnly=*/false);
    if (isSingleHomeRoot(roots)) {
        const QJsonArray homeMappedRoots =
            rootsFromHomeDirectories(settings, /*embedOnly=*/false);
        if (!homeMappedRoots.isEmpty()) {
            roots = homeMappedRoots;
        }
    }

    if (roots.isEmpty()) {
        roots = defaultCuratedRoots();
    }

    return roots;
}

QJsonArray ServiceManager::loadEmbeddingRoots() const
{
    const QJsonObject settings = readAppSettings();

    QJsonArray roots = rootsFromIndexRoots(settings, /*embedOnly=*/true);
    if (isSingleHomeRoot(rootsFromIndexRoots(settings, /*embedOnly=*/false))) {
        const QJsonArray homeMappedRoots =
            rootsFromHomeDirectories(settings, /*embedOnly=*/true);
        if (!homeMappedRoots.isEmpty()) {
            roots = homeMappedRoots;
        }
    }

    if (roots.isEmpty()) {
        const QJsonArray curated = defaultCuratedRoots();
        for (const QJsonValue& value : curated) {
            roots.append(value);
        }
    }

    return roots;
}

QString ServiceManager::findServiceBinary(const QString& name) const
{
    // Binary name matches CMake target: betterspotlight-<name>
    QString binaryName = QStringLiteral("betterspotlight-%1").arg(name);

    QString appDir = QCoreApplication::applicationDirPath();

    // Strategy 1: Same directory as app binary (installed bundle)
    QString bundlePath = appDir + QStringLiteral("/") + binaryName;
    if (QFileInfo::exists(bundlePath)) {
        return bundlePath;
    }

    // Strategy 2: ../Helpers/ inside the bundle
    QString helpersPath = appDir + QStringLiteral("/../Helpers/") + binaryName;
    if (QFileInfo::exists(helpersPath)) {
        return QFileInfo(helpersPath).canonicalFilePath();
    }

    // Strategy 3: CMake build directory layout — binaries are in
    //   build/src/services/<name>/betterspotlight-<name>
    // relative to the app at build/src/app/betterspotlight.app/Contents/MacOS/
    QStringList candidates = {
        // From .app/Contents/MacOS/ → ../../../../services/<name>/
        appDir + QStringLiteral("/../../../../services/%1/%2").arg(name, binaryName),
        // From a flat build dir
        appDir + QStringLiteral("/../../../services/%1/%2").arg(name, binaryName),
        // Sibling directory
        appDir + QStringLiteral("/../") + binaryName,
    };

    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).canonicalFilePath();
        }
    }

    LOG_WARN(bsCore, "ServiceManager: binary '%s' not found in any search path",
             qPrintable(binaryName));
    return {};
}

void ServiceManager::updateServiceStatus(const QString& name, const QString& status)
{
    if (name == QLatin1String("indexer")) {
        m_indexerStatus = status;
    } else if (name == QLatin1String("extractor")) {
        m_extractorStatus = status;
    } else if (name == QLatin1String("query")) {
        m_queryStatus = status;
    }
    emit serviceStatusChanged();
}

} // namespace bs
