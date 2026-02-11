#pragma once

#include "core/ipc/service_base.h"
#include "core/indexing/pipeline.h"
#include "core/index/sqlite_store.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"

#include <QFileSystemWatcher>

#include <optional>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <thread>

namespace bs {

class IndexerService : public ServiceBase {
    Q_OBJECT
public:
    explicit IndexerService(QObject* parent = nullptr);
    ~IndexerService() override;

protected:
    QJsonObject handleRequest(const QJsonObject& request) override;

private:
    // Method handlers (doc 05)
    QJsonObject handleStartIndexing(uint64_t id, const QJsonObject& params);
    QJsonObject handlePauseIndexing(uint64_t id);
    QJsonObject handleResumeIndexing(uint64_t id);
    QJsonObject handleSetUserActive(uint64_t id, const QJsonObject& params);
    QJsonObject handleReindexPath(uint64_t id, const QJsonObject& params);
    QJsonObject handleRebuildAll(uint64_t id);
    QJsonObject handleGetQueueStatus(uint64_t id);
    void joinRebuildThreadIfNeeded();
    void configureBsignoreWatcher();
    void reloadBsignore();
    QJsonObject bsignoreStatusJson() const;

private slots:
    void onBsignorePathChanged(const QString& path);
    void onBsignoreDirectoryChanged(const QString& path);

private:
    // Owned components
    std::optional<SQLiteStore> m_store;
    std::unique_ptr<ExtractionManager> m_extractor;
    PathRules m_pathRules;
    std::unique_ptr<Pipeline> m_pipeline;

    bool m_isIndexing = false;
    std::atomic<bool> m_rebuildRunning{false};
    std::atomic<bool> m_rebuildAwaitingDrain{false};
    std::atomic<qint64> m_rebuildStartedAtMs{0};
    std::atomic<qint64> m_rebuildFinishedAtMs{0};
    std::thread m_rebuildThread;
    bool m_lastQueueActive = false;

    // Stored roots for rebuild
    std::vector<std::string> m_currentRoots;

    std::unique_ptr<QFileSystemWatcher> m_bsignoreWatcher;
    QString m_bsignorePath;
    bool m_bsignoreLoaded = false;
    int m_bsignorePatternCount = 0;
    qint64 m_bsignoreLastLoadedAtMs = 0;
};

} // namespace bs
