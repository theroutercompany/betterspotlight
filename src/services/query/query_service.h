#pragma once

#include "core/ipc/service_base.h"
#include "core/index/sqlite_store.h"
#include "core/ranking/scorer.h"
#include "core/ranking/match_classifier.h"

#include <memory>
#include <optional>

namespace bs {

class EmbeddingManager;
class EmbeddingPipeline;
class InteractionTracker;
class FeedbackAggregator;
class PathPreferences;
class TypeAffinity;
class VectorIndex;
class VectorStore;
class SearchMerger;

class QueryService : public ServiceBase {
    Q_OBJECT
public:
    explicit QueryService(QObject* parent = nullptr);
    ~QueryService() override;

protected:
    QJsonObject handleRequest(const QJsonObject& request) override;

private:
    // ── M1 handlers ──
    QJsonObject handleSearch(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetHealth(uint64_t id);
    QJsonObject handleRecordFeedback(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetFrequency(uint64_t id, const QJsonObject& params);

    // ── M2 handlers ──
    QJsonObject handleRecordInteraction(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetPathPreferences(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetFileTypeAffinity(uint64_t id);
    QJsonObject handleRunAggregation(uint64_t id);
    QJsonObject handleExportInteractionData(uint64_t id, const QJsonObject& params);
    QJsonObject handleRebuildVectorIndex(uint64_t id);

    // ── Store + services ──
    std::optional<SQLiteStore> m_store;
    Scorer m_scorer;

    // M2 modules — initialized lazily after store open
    std::unique_ptr<InteractionTracker> m_interactionTracker;
    std::unique_ptr<FeedbackAggregator> m_feedbackAggregator;
    std::unique_ptr<PathPreferences> m_pathPreferences;
    std::unique_ptr<TypeAffinity> m_typeAffinity;
    std::unique_ptr<EmbeddingManager> m_embeddingManager;
    std::unique_ptr<VectorIndex> m_vectorIndex;
    std::unique_ptr<VectorStore> m_vectorStore;

    // Opens the store if not already open. Returns true on success.
    bool ensureStoreOpen();

    // Initialize M2 modules after store is opened.
    void initM2Modules();
    bool m_m2Initialized = false;
};

} // namespace bs
