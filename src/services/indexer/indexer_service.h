#pragma once

#include "core/ipc/service_base.h"
#include "core/indexing/pipeline.h"
#include "core/index/sqlite_store.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"

#include <optional>
#include <memory>
#include <vector>
#include <string>

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

    // Owned components
    std::optional<SQLiteStore> m_store;
    std::unique_ptr<ExtractionManager> m_extractor;
    PathRules m_pathRules;
    std::unique_ptr<Pipeline> m_pipeline;

    bool m_isIndexing = false;

    // Stored roots for rebuild
    std::vector<std::string> m_currentRoots;
};

} // namespace bs
