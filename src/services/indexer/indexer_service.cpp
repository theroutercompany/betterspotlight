#include "indexer_service.h"
#include "core/ipc/message.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>
#include <cinttypes>

namespace bs {

IndexerService::IndexerService(QObject* parent)
    : ServiceBase(QStringLiteral("indexer"), parent)
{
    LOG_INFO(bsIpc, "IndexerService created");
}

IndexerService::~IndexerService()
{
    if (m_pipeline) {
        m_pipeline->stop();
    }
}

QJsonObject IndexerService::handleRequest(const QJsonObject& request)
{
    QString method = request.value(QStringLiteral("method")).toString();
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
    QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("startIndexing"))  return handleStartIndexing(id, params);
    if (method == QLatin1String("pauseIndexing"))   return handlePauseIndexing(id);
    if (method == QLatin1String("resumeIndexing"))  return handleResumeIndexing(id);
    if (method == QLatin1String("setUserActive"))   return handleSetUserActive(id, params);
    if (method == QLatin1String("reindexPath"))     return handleReindexPath(id, params);
    if (method == QLatin1String("rebuildAll"))       return handleRebuildAll(id);
    if (method == QLatin1String("getQueueStatus"))  return handleGetQueueStatus(id);

    // Fall through to base (ping, shutdown, unknown)
    return ServiceBase::handleRequest(request);
}

QJsonObject IndexerService::handleStartIndexing(uint64_t id, const QJsonObject& params)
{
    if (m_isIndexing) {
        return IpcMessage::makeError(id, IpcErrorCode::AlreadyRunning,
                                     QStringLiteral("Indexing is already running"));
    }

    // Parse roots array from params
    QJsonArray rootsArray = params.value(QStringLiteral("roots")).toArray();
    if (rootsArray.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing or empty 'roots' array"));
    }

    std::vector<std::string> roots;
    roots.reserve(static_cast<size_t>(rootsArray.size()));
    for (const auto& val : rootsArray) {
        QString root = val.toString();
        if (root.isEmpty()) {
            continue;
        }
        roots.push_back(root.toStdString());
    }

    if (roots.empty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("No valid roots provided"));
    }

    // Open/create SQLiteStore at default path
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/betterspotlight");
    QDir().mkpath(dataDir);
    QString dbPath = dataDir + QStringLiteral("/index.db");

    auto store = SQLiteStore::open(dbPath);
    if (!store.has_value()) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to open database at: %1").arg(dbPath));
    }
    m_store.emplace(std::move(store.value()));

    // Load user-defined exclusion patterns from ~/.bsignore and start watching
    // for live updates so exclusions apply without restarting services.
    m_bsignorePath = QDir::homePath() + QStringLiteral("/.bsignore");
    m_bsignoreLoaded = m_pathRules.loadBsignore(m_bsignorePath.toStdString());
    m_bsignorePatternCount = static_cast<int>(m_pathRules.bsignorePatternCount());
    m_bsignoreLastLoadedAtMs = m_pathRules.bsignoreLastLoadedAtMs();
    configureBsignoreWatcher();
    m_pathRules.setExplicitIncludeRoots(roots);

    // Create ExtractionManager and Pipeline
    m_extractor = std::make_unique<ExtractionManager>();

    // Apply extraction limits from database settings
    if (auto maxSizeStr = m_store->getSetting(QStringLiteral("max_file_size"))) {
        bool ok = false;
        int64_t maxSize = maxSizeStr->toLongLong(&ok);
        if (ok && maxSize > 0) {
            m_extractor->setMaxFileSizeBytes(maxSize);
            LOG_INFO(bsIpc, "Extraction max file size: %" PRId64 " bytes", maxSize);
        }
    }

    if (auto timeoutStr = m_store->getSetting(QStringLiteral("extraction_timeout_ms"))) {
        bool ok = false;
        int timeoutMs = timeoutStr->toInt(&ok);
        if (ok && timeoutMs > 0) {
            m_extractor->setTimeoutMs(timeoutMs);
            LOG_INFO(bsIpc, "Extraction timeout: %d ms", timeoutMs);
        }
    }

    m_pipeline = std::make_unique<Pipeline>(m_store.value(), *m_extractor, m_pathRules);

    // Connect pipeline signals to IPC notifications
    connect(m_pipeline.get(), &Pipeline::progressUpdated,
            this, [this](int processed, int total) {
        QJsonObject params;
        params[QStringLiteral("scanned")] = processed;
        params[QStringLiteral("total")] = total;
        params[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        sendNotification(QStringLiteral("indexingProgress"), params);
    });

    connect(m_pipeline.get(), &Pipeline::indexingComplete,
            this, [this]() {
        QJsonObject params;
        params[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        sendNotification(QStringLiteral("indexingComplete"), params);
    });

    connect(m_pipeline.get(), &Pipeline::indexingError,
            this, [this](const QString& error) {
        QJsonObject params;
        params[QStringLiteral("error")] = error;
        params[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        sendNotification(QStringLiteral("indexingError"), params);
    });

    // Start the pipeline
    m_pipeline->start(roots);
    m_currentRoots = roots;
    m_isIndexing = true;

    LOG_INFO(bsIpc, "Indexing started with %d root(s)", static_cast<int>(roots.size()));

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("queuedPaths")] = static_cast<qint64>(m_pipeline->queueStatus().depth);
    result[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
    return IpcMessage::makeResponse(id, result);
}

QJsonObject IndexerService::handlePauseIndexing(uint64_t id)
{
    if (!m_isIndexing || !m_pipeline) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Indexing is not running"));
    }

    m_pipeline->pause();

    LOG_INFO(bsIpc, "Indexing paused");

    QJsonObject result;
    result[QStringLiteral("paused")] = true;
    result[QStringLiteral("queuedPaths")] = static_cast<qint64>(m_pipeline->queueStatus().depth);
    return IpcMessage::makeResponse(id, result);
}

QJsonObject IndexerService::handleResumeIndexing(uint64_t id)
{
    if (!m_isIndexing || !m_pipeline) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Indexing is not running"));
    }

    m_pipeline->resume();

    LOG_INFO(bsIpc, "Indexing resumed");

    QJsonObject result;
    result[QStringLiteral("resumed")] = true;
    result[QStringLiteral("queuedPaths")] = static_cast<qint64>(m_pipeline->queueStatus().depth);
    return IpcMessage::makeResponse(id, result);
}

QJsonObject IndexerService::handleSetUserActive(uint64_t id, const QJsonObject& params)
{
    if (!m_isIndexing || !m_pipeline) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Indexing is not running"));
    }

    if (!params.contains(QStringLiteral("active"))) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'active' parameter"));
    }

    const bool active = params.value(QStringLiteral("active")).toBool(false);
    m_pipeline->setUserActive(active);

    QJsonObject result;
    result[QStringLiteral("active")] = active;
    result[QStringLiteral("prepWorkers")] =
        static_cast<qint64>(m_pipeline->queueStatus().prepWorkers);
    return IpcMessage::makeResponse(id, result);
}

QJsonObject IndexerService::handleReindexPath(uint64_t id, const QJsonObject& params)
{
    QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'path' parameter"));
    }

    if (!m_isIndexing || !m_pipeline) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Indexing is not running"));
    }

    m_pipeline->reindexPath(path);

    LOG_INFO(bsIpc, "Reindex queued for path: %s", qPrintable(path));

    QJsonObject result;
    result[QStringLiteral("queued")] = true;
    result[QStringLiteral("deletedEntries")] = 0;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject IndexerService::handleRebuildAll(uint64_t id)
{
    if (!m_isIndexing || !m_pipeline) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Indexing is not running; call startIndexing first"));
    }

    m_pipeline->rebuildAll(m_currentRoots);

    LOG_INFO(bsIpc, "Rebuild all initiated");

    QJsonObject result;
    result[QStringLiteral("cleared")] = true;
    result[QStringLiteral("deletedEntries")] = 0;
    result[QStringLiteral("reindexingStarted")] = true;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject IndexerService::handleGetQueueStatus(uint64_t id)
{
    QJsonArray roots;
    for (const std::string& root : m_currentRoots) {
        roots.append(QString::fromStdString(root));
    }

    if (!m_pipeline) {
        // Return zeroed stats when pipeline is not yet created
        QJsonObject lastProgress;
        lastProgress[QStringLiteral("scanned")] = 0;
        lastProgress[QStringLiteral("total")] = 0;

        QJsonObject result;
        result[QStringLiteral("pending")] = 0;
        result[QStringLiteral("processing")] = 0;
        result[QStringLiteral("failed")] = 0;
        result[QStringLiteral("dropped")] = 0;
        result[QStringLiteral("paused")] = false;
        result[QStringLiteral("roots")] = roots;
        result[QStringLiteral("lastProgressReport")] = lastProgress;
        const QJsonObject bsignore = bsignoreStatusJson();
        result[QStringLiteral("bsignorePath")] = bsignore.value(QStringLiteral("path")).toString();
        result[QStringLiteral("bsignoreLoaded")] = bsignore.value(QStringLiteral("loaded")).toBool(false);
        result[QStringLiteral("bsignorePatternCount")] =
            bsignore.value(QStringLiteral("patternCount")).toInt(0);
        result[QStringLiteral("bsignoreLastLoadedAtMs")] =
            bsignore.value(QStringLiteral("lastLoadedAtMs")).toInteger();
        return IpcMessage::makeResponse(id, result);
    }

    QueueStats stats = m_pipeline->queueStatus();
    int processed = m_pipeline->processedCount();

    QJsonObject lastProgress;
    lastProgress[QStringLiteral("scanned")] = processed;
    lastProgress[QStringLiteral("total")] = processed + static_cast<int>(stats.depth);

    QJsonObject result;
    result[QStringLiteral("pending")] = static_cast<qint64>(stats.depth);
    result[QStringLiteral("processing")] = static_cast<qint64>(stats.activeItems);
    result[QStringLiteral("failed")] = static_cast<qint64>(stats.failedItems);
    result[QStringLiteral("dropped")] = static_cast<qint64>(stats.droppedItems);
    result[QStringLiteral("paused")] = stats.isPaused;
    result[QStringLiteral("preparing")] = static_cast<qint64>(stats.preparing);
    result[QStringLiteral("writing")] = static_cast<qint64>(stats.writing);
    result[QStringLiteral("coalesced")] = static_cast<qint64>(stats.coalesced);
    result[QStringLiteral("staleDropped")] = static_cast<qint64>(stats.staleDropped);
    result[QStringLiteral("prepWorkers")] = static_cast<qint64>(stats.prepWorkers);
    result[QStringLiteral("writerBatchDepth")] = static_cast<qint64>(stats.writerBatchDepth);
    result[QStringLiteral("roots")] = roots;
    result[QStringLiteral("lastProgressReport")] = lastProgress;
    const QJsonObject bsignore = bsignoreStatusJson();
    result[QStringLiteral("bsignorePath")] = bsignore.value(QStringLiteral("path")).toString();
    result[QStringLiteral("bsignoreLoaded")] = bsignore.value(QStringLiteral("loaded")).toBool(false);
    result[QStringLiteral("bsignorePatternCount")] =
        bsignore.value(QStringLiteral("patternCount")).toInt(0);
    result[QStringLiteral("bsignoreLastLoadedAtMs")] =
        bsignore.value(QStringLiteral("lastLoadedAtMs")).toInteger();
    return IpcMessage::makeResponse(id, result);
}

void IndexerService::configureBsignoreWatcher()
{
    if (m_bsignorePath.isEmpty()) {
        return;
    }

    if (!m_bsignoreWatcher) {
        m_bsignoreWatcher = std::make_unique<QFileSystemWatcher>(this);
        connect(m_bsignoreWatcher.get(), &QFileSystemWatcher::fileChanged,
                this, &IndexerService::onBsignorePathChanged);
        connect(m_bsignoreWatcher.get(), &QFileSystemWatcher::directoryChanged,
                this, &IndexerService::onBsignoreDirectoryChanged);
    }

    if (!m_bsignoreWatcher->files().isEmpty()) {
        m_bsignoreWatcher->removePaths(m_bsignoreWatcher->files());
    }
    if (!m_bsignoreWatcher->directories().isEmpty()) {
        m_bsignoreWatcher->removePaths(m_bsignoreWatcher->directories());
    }

    const QFileInfo info(m_bsignorePath);
    const QString parentDir = info.absoluteDir().absolutePath();
    if (QFileInfo::exists(parentDir)) {
        m_bsignoreWatcher->addPath(parentDir);
    }
    if (info.exists()) {
        m_bsignoreWatcher->addPath(m_bsignorePath);
    }
}

void IndexerService::reloadBsignore()
{
    if (m_bsignorePath.isEmpty()) {
        return;
    }

    m_bsignoreLoaded = m_pathRules.loadBsignore(m_bsignorePath.toStdString());
    m_bsignorePatternCount = static_cast<int>(m_pathRules.bsignorePatternCount());
    m_bsignoreLastLoadedAtMs = m_pathRules.bsignoreLastLoadedAtMs();
    configureBsignoreWatcher();

    QJsonObject params = bsignoreStatusJson();
    params[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentMSecsSinceEpoch());
    sendNotification(QStringLiteral("bsignoreReloaded"), params);
}

QJsonObject IndexerService::bsignoreStatusJson() const
{
    QJsonObject status;
    status[QStringLiteral("path")] = m_bsignorePath;
    status[QStringLiteral("loaded")] = m_bsignoreLoaded;
    status[QStringLiteral("patternCount")] = m_bsignorePatternCount;
    status[QStringLiteral("lastLoadedAtMs")] = m_bsignoreLastLoadedAtMs;
    status[QStringLiteral("lastLoadedAt")] = m_bsignoreLastLoadedAtMs > 0
        ? QDateTime::fromMSecsSinceEpoch(m_bsignoreLastLoadedAtMs).toUTC().toString(Qt::ISODate)
        : QString();
    return status;
}

void IndexerService::onBsignorePathChanged(const QString& /*path*/)
{
    reloadBsignore();
}

void IndexerService::onBsignoreDirectoryChanged(const QString& /*path*/)
{
    reloadBsignore();
}

} // namespace bs
