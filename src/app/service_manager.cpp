#include "service_manager.h"
#include "core/models/model_manifest.h"
#include "core/models/model_registry.h"
#include "core/shared/logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

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

bool isErrorStatus(const QString& status)
{
    return status == QLatin1String("error") || status == QLatin1String("crashed");
}

bool readBoolSetting(const QJsonObject& settings, const QString& key, bool fallback)
{
    if (!settings.contains(key)) {
        return fallback;
    }
    const QJsonValue value = settings.value(key);
    if (value.isBool()) {
        return value.toBool(fallback);
    }
    if (value.isDouble()) {
        return value.toDouble(fallback ? 1.0 : 0.0) != 0.0;
    }
    if (value.isString()) {
        const QString normalized = value.toString().trimmed().toLower();
        if (normalized == QLatin1String("1")
            || normalized == QLatin1String("true")
            || normalized == QLatin1String("on")
            || normalized == QLatin1String("yes")) {
            return true;
        }
        if (normalized == QLatin1String("0")
            || normalized == QLatin1String("false")
            || normalized == QLatin1String("off")
            || normalized == QLatin1String("no")) {
            return false;
        }
    }
    return fallback;
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

QStringList modelDownloadUrlsForRole(const QString& role)
{
    if (role == QLatin1String("bi-encoder")) {
        return {
            QStringLiteral("https://huggingface.co/Xenova/bge-large-en-v1.5/resolve/main/onnx/model.onnx"),
        };
    }
    if (role == QLatin1String("bi-encoder-legacy")) {
        return {
            QStringLiteral("https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/onnx/model_int8.onnx"),
            QStringLiteral("https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/onnx/model.onnx"),
        };
    }
    if (role == QLatin1String("bi-encoder-fast")) {
        return {
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-embed-xsmall-v1/resolve/main/onnx/model_int8.onnx"),
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-embed-xsmall-v1/resolve/main/onnx/model.onnx"),
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-embed-xsmall-v1/resolve/main/model_int8.onnx"),
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-embed-xsmall-v1/resolve/main/model.onnx"),
        };
    }
    if (role == QLatin1String("cross-encoder")) {
        return {
            QStringLiteral("https://huggingface.co/cross-encoder/ms-marco-MiniLM-L-6-v2/resolve/main/onnx/model_int8.onnx"),
            QStringLiteral("https://huggingface.co/cross-encoder/ms-marco-MiniLM-L-6-v2/resolve/main/onnx/model.onnx"),
        };
    }
    if (role == QLatin1String("cross-encoder-fast")) {
        return {
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-rerank-xsmall-v1/resolve/main/onnx/model_int8.onnx"),
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-rerank-xsmall-v1/resolve/main/onnx/model.onnx"),
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-rerank-xsmall-v1/resolve/main/model_int8.onnx"),
            QStringLiteral("https://huggingface.co/mixedbread-ai/mxbai-rerank-xsmall-v1/resolve/main/model.onnx"),
        };
    }
    if (role == QLatin1String("qa-extractive")) {
        return {
            QStringLiteral("https://huggingface.co/Xenova/distilbert-base-cased-distilled-squad/resolve/main/onnx/model_quantized.onnx"),
            QStringLiteral("https://huggingface.co/Xenova/distilbert-base-cased-distilled-squad/resolve/main/onnx/model.onnx"),
            QStringLiteral("https://huggingface.co/distilbert/distilbert-base-cased-distilled-squad/resolve/main/onnx/model.onnx"),
        };
    }

    // query-router and unknown roles intentionally require manual provisioning.
    return {};
}

QStringList vocabDownloadUrls()
{
    return {
        QStringLiteral("https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/vocab.txt"),
    };
}

QString trimSingleLine(const QString& text, int maxChars = 220)
{
    QString normalized = text.simplified();
    if (normalized.size() <= maxChars) {
        return normalized;
    }
    return normalized.left(std::max(0, maxChars - 3)) + QStringLiteral("...");
}

bool downloadFileWithCurl(const QString& url, const QString& outputPath, QString* errorOut)
{
    if (errorOut) {
        *errorOut = QString();
    }

    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    const QString tmpPath = outputPath + QStringLiteral(".tmp");
    QFile::remove(tmpPath);

    QProcess process;
    const QStringList args = {
        QStringLiteral("-fL"),
        QStringLiteral("--retry"), QStringLiteral("3"),
        QStringLiteral("--retry-delay"), QStringLiteral("2"),
        QStringLiteral("--connect-timeout"), QStringLiteral("20"),
        QStringLiteral("--max-time"), QStringLiteral("1800"),
        url,
        QStringLiteral("-o"),
        tmpPath,
    };

    process.start(QStringLiteral("/usr/bin/curl"), args);
    if (!process.waitForStarted(5000)) {
        if (errorOut) {
            *errorOut = QStringLiteral("failed to start curl");
        }
        QFile::remove(tmpPath);
        return false;
    }
    process.waitForFinished(-1);

    const QString stdErr = QString::fromUtf8(process.readAllStandardError());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            *errorOut = trimSingleLine(stdErr.isEmpty()
                                           ? QStringLiteral("curl exited with code %1")
                                                 .arg(process.exitCode())
                                           : stdErr);
        }
        QFile::remove(tmpPath);
        return false;
    }

    const QFileInfo tmpInfo(tmpPath);
    if (!tmpInfo.exists() || tmpInfo.size() <= 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("downloaded file is empty");
        }
        QFile::remove(tmpPath);
        return false;
    }

    QFile::remove(outputPath);
    if (!QFile::rename(tmpPath, outputPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("failed to move downloaded file into place");
        }
        QFile::remove(tmpPath);
        return false;
    }

    return true;
}

} // namespace

ServiceManager::ServiceManager(QObject* parent)
    : QObject(parent)
    , m_supervisor(std::make_unique<Supervisor>(this))
    , m_indexerStatus(QStringLiteral("stopped"))
    , m_extractorStatus(QStringLiteral("stopped"))
    , m_queryStatus(QStringLiteral("stopped"))
    , m_inferenceStatus(QStringLiteral("stopped"))
{
    connect(m_supervisor.get(), &Supervisor::serviceStarted,
            this, &ServiceManager::onServiceStarted);
    connect(m_supervisor.get(), &Supervisor::serviceStopped,
            this, &ServiceManager::onServiceStopped);
    connect(m_supervisor.get(), &Supervisor::serviceCrashed,
            this, &ServiceManager::onServiceCrashed);
    connect(m_supervisor.get(), &Supervisor::allServicesReady,
            this, &ServiceManager::onAllServicesReady);

    m_indexingStatusTimer.setInterval(2000);
    connect(&m_indexingStatusTimer, &QTimer::timeout,
            this, &ServiceManager::refreshIndexerQueueStatus);
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

QString ServiceManager::inferenceStatus() const
{
    return m_inferenceStatus;
}

QString ServiceManager::trayState() const
{
    return trayStateToString(m_trayState);
}

bool ServiceManager::modelDownloadRunning() const
{
    std::lock_guard<std::mutex> lock(m_modelDownloadMutex);
    return m_modelDownloadRunning;
}

QString ServiceManager::modelDownloadStatus() const
{
    std::lock_guard<std::mutex> lock(m_modelDownloadMutex);
    return m_modelDownloadStatus;
}

bool ServiceManager::modelDownloadHasError() const
{
    std::lock_guard<std::mutex> lock(m_modelDownloadMutex);
    return m_modelDownloadHasError;
}

Supervisor* ServiceManager::supervisor() const
{
    return m_supervisor.get();
}

void ServiceManager::start()
{
    LOG_INFO(bsCore, "ServiceManager: starting services");
    m_initialIndexingStarted = false;
    m_indexingActive = false;
    m_lastQueueRebuildRunning = false;
    m_lastQueueRebuildFinishedAtMs = 0;
    m_pendingPostRebuildVectorRefresh = false;
    m_pendingPostRebuildVectorRefreshAttempts = 0;
    setModelDownloadState(/*running=*/false, QString(), /*hasError=*/false);

    // Register all three service binaries
    const QStringList serviceNames = {
        QStringLiteral("indexer"),
        QStringLiteral("extractor"),
        QStringLiteral("query"),
        QStringLiteral("inference"),
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
    updateTrayState();
}

void ServiceManager::stop()
{
    joinModelDownloadThreadIfNeeded();
    LOG_INFO(bsCore, "ServiceManager: stopping services");
    m_supervisor->stopAll();

    m_allReady = false;
    m_initialIndexingStarted = false;
    m_indexingActive = false;
    m_lastQueueRebuildRunning = false;
    m_lastQueueRebuildFinishedAtMs = 0;
    m_pendingPostRebuildVectorRefresh = false;
    m_pendingPostRebuildVectorRefreshAttempts = 0;
    setModelDownloadState(/*running=*/false, modelDownloadStatus(), /*hasError=*/false);
    m_indexingStatusTimer.stop();
    m_indexerStatus = QStringLiteral("stopped");
    m_extractorStatus = QStringLiteral("stopped");
    m_queryStatus = QStringLiteral("stopped");
    m_inferenceStatus = QStringLiteral("stopped");
    emit serviceStatusChanged();
    updateTrayState();
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
    if (name == QLatin1String("indexer")) {
        m_indexingActive = false;
    }
    m_indexingStatusTimer.stop();
    updateServiceStatus(name, QStringLiteral("stopped"));
}

void ServiceManager::onServiceCrashed(const QString& name, int crashCount)
{
    LOG_WARN(bsCore, "ServiceManager: service '%s' crashed (count=%d)",
             qPrintable(name), crashCount);
    m_allReady = false;
    if (name == QLatin1String("indexer")) {
        m_indexingActive = false;
    }
    m_indexingStatusTimer.stop();
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
    if (!m_indexingStatusTimer.isActive()) {
        m_indexingStatusTimer.start();
    }
    refreshIndexerQueueStatus();
    updateTrayState();
}

void ServiceManager::startIndexing()
{
    QJsonObject params;
    params[QStringLiteral("roots")] = loadIndexRoots();

    LOG_INFO(bsCore, "ServiceManager: sending startIndexing (%d root(s))",
             static_cast<int>(params.value(QStringLiteral("roots")).toArray().size()));
    if (sendIndexerRequest(QStringLiteral("startIndexing"), params)) {
        m_indexingActive = true;
        updateTrayState();
    }
}

bool ServiceManager::pauseIndexing()
{
    if (sendIndexerRequest(QStringLiteral("pauseIndexing"))) {
        m_indexingActive = false;
        updateTrayState();
        return true;
    }
    return false;
}

bool ServiceManager::resumeIndexing()
{
    if (sendIndexerRequest(QStringLiteral("resumeIndexing"))) {
        m_indexingActive = true;
        updateTrayState();
        return true;
    }
    return false;
}

void ServiceManager::setIndexingUserActive(bool active)
{
    QJsonObject params;
    params[QStringLiteral("active")] = active;
    sendIndexerRequest(QStringLiteral("setUserActive"), params);
}

bool ServiceManager::rebuildAll()
{
    if (sendIndexerRequest(QStringLiteral("rebuildAll"))) {
        m_indexingActive = true;
        updateTrayState();
        return true;
    }
    return false;
}

bool ServiceManager::rebuildVectorIndex()
{
    SocketClient* client = m_supervisor->clientFor(QStringLiteral("query"));
    if (!client || !client->isConnected()) {
        LOG_WARN(bsCore, "ServiceManager: query not connected, can't send 'rebuildVectorIndex'");
        return false;
    }

    QJsonObject params;
    const QJsonArray embedRoots = loadEmbeddingRoots();
    if (!embedRoots.isEmpty()) {
        params[QStringLiteral("includePaths")] = embedRoots;
    }

    auto response = client->sendRequest(QStringLiteral("rebuildVectorIndex"), params, 10000);
    if (!response) {
        LOG_ERROR(bsCore, "ServiceManager: query request failed: rebuildVectorIndex");
        return false;
    }

    const QString type = response->value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        const QString msg = response->value(QStringLiteral("error")).toObject()
                                .value(QStringLiteral("message")).toString();
        LOG_ERROR(bsCore, "ServiceManager: query 'rebuildVectorIndex' error: %s",
                  qPrintable(msg));
        emit serviceError(QStringLiteral("query"), msg);
        return false;
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
    return true;
}

bool ServiceManager::clearExtractionCache()
{
    return sendServiceRequest(QStringLiteral("extractor"), QStringLiteral("clearExtractionCache"));
}

bool ServiceManager::reindexPath(const QString& path)
{
    QString normalizedPath = path;
    if (normalizedPath.startsWith(QStringLiteral("file://"))) {
        normalizedPath = QUrl(normalizedPath).toLocalFile();
    }
    QJsonObject params;
    params[QStringLiteral("path")] = normalizedPath;
    if (sendIndexerRequest(QStringLiteral("reindexPath"), params)) {
        m_indexingActive = true;
        updateTrayState();
        return true;
    }
    return false;
}

bool ServiceManager::downloadModels(const QStringList& roles, bool includeExisting)
{
    {
        std::lock_guard<std::mutex> lock(m_modelDownloadMutex);
        if (m_modelDownloadRunning) {
            return false;
        }
    }

    setModelDownloadState(/*running=*/true,
                          QStringLiteral("Preparing model download plan..."),
                          /*hasError=*/false);

    joinModelDownloadThreadIfNeeded();
    m_modelDownloadThread = std::thread(
        &ServiceManager::runModelDownloadWorker, this, roles, includeExisting);
    return true;
}

void ServiceManager::runModelDownloadWorker(const QStringList& roles, bool includeExisting)
{
    const auto publish = [this](bool running,
                                const QString& status,
                                bool hasError) {
        QMetaObject::invokeMethod(
            this,
            [this, running, status, hasError]() {
                setModelDownloadState(running, status, hasError);
            },
            Qt::QueuedConnection);
    };

    const QString modelsDir = ModelRegistry::resolveModelsDir();
    const QString manifestPath = modelsDir + QStringLiteral("/manifest.json");
    const std::optional<ModelManifest> manifestOpt = ModelManifest::loadFromFile(manifestPath);
    if (!manifestOpt.has_value()) {
        publish(/*running=*/false,
                QStringLiteral("Model download failed: could not load manifest at %1")
                    .arg(manifestPath),
                /*hasError=*/true);
        return;
    }

    QStringList targetRoles;
    QSet<QString> requestedRoles;
    for (const QString& roleRaw : roles) {
        const QString role = roleRaw.trimmed();
        if (!role.isEmpty()) {
            requestedRoles.insert(role);
        }
    }
    if (requestedRoles.isEmpty()) {
        for (const auto& pair : manifestOpt->models) {
            targetRoles.append(QString::fromStdString(pair.first));
        }
    } else {
        targetRoles = requestedRoles.values();
    }
    std::sort(targetRoles.begin(), targetRoles.end(),
              [](const QString& lhs, const QString& rhs) {
                  return lhs.toLower() < rhs.toLower();
              });

    if (targetRoles.isEmpty()) {
        publish(/*running=*/false,
                QStringLiteral("No model roles selected."),
                /*hasError=*/true);
        return;
    }

    int downloadedCount = 0;
    int skippedCount = 0;
    QStringList failures;
    bool vocabChecked = false;
    bool vocabReady = false;

    for (int idx = 0; idx < targetRoles.size(); ++idx) {
        const QString role = targetRoles.at(idx);
        const auto manifestIt = manifestOpt->models.find(role.toStdString());
        if (manifestIt == manifestOpt->models.end()) {
            failures.append(QStringLiteral("%1: role not found in manifest").arg(role));
            continue;
        }

        const ModelManifestEntry& entry = manifestIt->second;
        const QString modelPath = modelsDir + QStringLiteral("/") + entry.file;
        const QFileInfo modelInfo(modelPath);
        if (!includeExisting && modelInfo.exists() && modelInfo.isReadable()
            && modelInfo.size() > 0) {
            ++skippedCount;
            continue;
        }

        publish(/*running=*/true,
                QStringLiteral("Downloading %1 (%2/%3)...")
                    .arg(role)
                    .arg(idx + 1)
                    .arg(targetRoles.size()),
                /*hasError=*/false);

        const QStringList urls = modelDownloadUrlsForRole(role);
        if (urls.isEmpty()) {
            failures.append(QStringLiteral("%1: no download source configured").arg(role));
            continue;
        }

        bool downloaded = false;
        QString lastError;
        for (const QString& url : urls) {
            QString attemptError;
            if (downloadFileWithCurl(url, modelPath, &attemptError)) {
                downloaded = true;
                break;
            }
            lastError = QStringLiteral("%1 (%2)").arg(attemptError, url);
        }
        if (downloaded) {
            ++downloadedCount;
        } else {
            failures.append(QStringLiteral("%1: %2")
                                .arg(role,
                                     lastError.isEmpty()
                                         ? QStringLiteral("download failed")
                                         : trimSingleLine(lastError)));
        }

        if (!vocabChecked && !entry.vocab.isEmpty()) {
            vocabChecked = true;
            const QString vocabPath = modelsDir + QStringLiteral("/") + entry.vocab;
            const QFileInfo vocabInfo(vocabPath);
            if (vocabInfo.exists() && vocabInfo.isReadable() && vocabInfo.size() > 0) {
                vocabReady = true;
            } else {
                publish(/*running=*/true,
                        QStringLiteral("Downloading tokenizer vocab..."),
                        /*hasError=*/false);
                for (const QString& url : vocabDownloadUrls()) {
                    QString attemptError;
                    if (downloadFileWithCurl(url, vocabPath, &attemptError)) {
                        vocabReady = true;
                        break;
                    }
                }
                if (!vocabReady) {
                    failures.append(QStringLiteral("vocab: failed to download %1")
                                        .arg(entry.vocab));
                }
            }
        }
    }

    const bool hasError = !failures.isEmpty();
    QString summary = QStringLiteral("Model download complete: %1 downloaded, %2 skipped")
                          .arg(downloadedCount)
                          .arg(skippedCount);
    if (hasError) {
        summary += QStringLiteral(", %1 failed (%2)")
                       .arg(failures.size())
                       .arg(trimSingleLine(failures.first()));
    }
    publish(/*running=*/false, summary, hasError);
}

void ServiceManager::joinModelDownloadThreadIfNeeded()
{
    if (!m_modelDownloadThread.joinable()) {
        return;
    }
    if (m_modelDownloadThread.get_id() == std::this_thread::get_id()) {
        return;
    }
    m_modelDownloadThread.join();
}

void ServiceManager::setModelDownloadState(bool running,
                                           const QString& status,
                                           bool hasError)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_modelDownloadMutex);
        if (m_modelDownloadRunning != running
            || m_modelDownloadStatus != status
            || m_modelDownloadHasError != hasError) {
            m_modelDownloadRunning = running;
            m_modelDownloadStatus = status;
            m_modelDownloadHasError = hasError;
            changed = true;
        }
    }
    if (changed) {
        emit modelDownloadStateChanged();
    }
}

void ServiceManager::triggerInitialIndexing()
{
    if (!m_allReady) {
        LOG_WARN(bsCore, "ServiceManager: triggerInitialIndexing ignored; services are not ready");
        return;
    }
    if (m_initialIndexingStarted) {
        return;
    }
    m_initialIndexingStarted = true;
    startIndexing();
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

    // Strategy 1: ../Helpers/ inside the bundle (release layout)
    QString helpersPath = appDir + QStringLiteral("/../Helpers/") + binaryName;
    if (QFileInfo::exists(helpersPath)) {
        return QFileInfo(helpersPath).canonicalFilePath();
    }

    // Strategy 2: Same directory as app binary (development fallback)
    QString bundlePath = appDir + QStringLiteral("/") + binaryName;
    if (QFileInfo::exists(bundlePath)) {
        return bundlePath;
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
    } else if (name == QLatin1String("inference")) {
        m_inferenceStatus = status;
    }
    emit serviceStatusChanged();
    updateTrayState();
}

QString ServiceManager::trayStateToString(TrayState state)
{
    switch (state) {
    case TrayState::Idle:
        return QStringLiteral("idle");
    case TrayState::Indexing:
        return QStringLiteral("indexing");
    case TrayState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

void ServiceManager::updateTrayState()
{
    TrayState nextState = TrayState::Idle;
    if (isErrorStatus(m_indexerStatus)
        || isErrorStatus(m_extractorStatus)
        || isErrorStatus(m_queryStatus)
        || isErrorStatus(m_inferenceStatus)) {
        nextState = TrayState::Error;
    } else if (!m_allReady
               || m_indexingActive
               || m_indexerStatus == QLatin1String("starting")
               || m_extractorStatus == QLatin1String("starting")
               || m_queryStatus == QLatin1String("starting")
               || m_inferenceStatus == QLatin1String("starting")) {
        nextState = TrayState::Indexing;
    }

    if (m_trayState == nextState) {
        return;
    }

    m_trayState = nextState;
    emit trayStateChanged();
}

void ServiceManager::refreshIndexerQueueStatus()
{
    if (!m_allReady || !m_supervisor) {
        return;
    }

    SocketClient* client = m_supervisor->clientFor(QStringLiteral("indexer"));
    if (!client || !client->isConnected()) {
        return;
    }

    auto response = client->sendRequest(QStringLiteral("getQueueStatus"), {}, 500);
    if (!response) {
        return;
    }
    if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
        return;
    }

    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    const qint64 pending = result.value(QStringLiteral("pending")).toInteger();
    const qint64 processing = result.value(QStringLiteral("processing")).toInteger();
    const qint64 preparing = result.value(QStringLiteral("preparing")).toInteger();
    const qint64 writing = result.value(QStringLiteral("writing")).toInteger();
    const bool rebuildRunning = result.value(QStringLiteral("rebuildRunning")).toBool(false);
    const qint64 rebuildFinishedAtMs =
        result.value(QStringLiteral("rebuildFinishedAtMs")).toInteger();
    const bool active = pending > 0 || processing > 0 || preparing > 0 || writing > 0;

    // Rebuild-All is a two-phase operation: filesystem indexing first, then vector
    // rebuild. Automatically trigger phase 2 after rebuild-all scan/index drain.
    if (m_lastQueueRebuildRunning && !rebuildRunning
        && rebuildFinishedAtMs > 0
        && rebuildFinishedAtMs != m_lastQueueRebuildFinishedAtMs) {
        const QJsonObject settings = readAppSettings();
        if (readBoolSetting(settings, QStringLiteral("autoVectorMigration"), true)) {
            m_pendingPostRebuildVectorRefresh = true;
            m_pendingPostRebuildVectorRefreshAttempts = 0;
            LOG_INFO(bsCore,
                     "ServiceManager: index rebuild completed at %lld, scheduling vector rebuild",
                     static_cast<long long>(rebuildFinishedAtMs));
        } else {
            LOG_INFO(bsCore,
                     "ServiceManager: index rebuild completed but auto vector migration is disabled");
        }
    }

    m_lastQueueRebuildRunning = rebuildRunning;
    if (rebuildFinishedAtMs > 0) {
        m_lastQueueRebuildFinishedAtMs = rebuildFinishedAtMs;
    }

    if (m_pendingPostRebuildVectorRefresh && !rebuildRunning) {
        if (m_pendingPostRebuildVectorRefreshAttempts >= 5) {
            LOG_WARN(bsCore,
                     "ServiceManager: giving up auto vector rebuild after %d attempts",
                     m_pendingPostRebuildVectorRefreshAttempts);
            m_pendingPostRebuildVectorRefresh = false;
            m_pendingPostRebuildVectorRefreshAttempts = 0;
        } else {
            ++m_pendingPostRebuildVectorRefreshAttempts;
            if (rebuildVectorIndex()) {
                LOG_INFO(bsCore,
                         "ServiceManager: auto vector rebuild triggered (attempt=%d)",
                         m_pendingPostRebuildVectorRefreshAttempts);
                m_pendingPostRebuildVectorRefresh = false;
                m_pendingPostRebuildVectorRefreshAttempts = 0;
            }
        }
    }

    if (m_indexingActive == active) {
        return;
    }

    m_indexingActive = active;
    updateTrayState();
}

} // namespace bs
