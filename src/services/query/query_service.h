#pragma once

#include "core/ipc/service_base.h"
#include "core/index/sqlite_store.h"
#include "core/index/typo_lexicon.h"
#include "core/fs/bsignore_parser.h"
#include "core/ranking/scorer.h"
#include "core/query/query_cache.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <QFileSystemWatcher>
#include <QStringList>
#include <shared_mutex>
#include <thread>

namespace bs {

class CrossEncoderReranker;
class EmbeddingManager;
class EmbeddingPipeline;
class ModelRegistry;
class InteractionTracker;
class FeedbackAggregator;
class PathPreferences;
class TypeAffinity;
class PersonalizedLtr;
class QaExtractiveModel;
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
    QJsonObject handleGetAnswerSnippet(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetHealth(uint64_t id);
    QJsonObject handleGetHealthDetails(uint64_t id, const QJsonObject& params);
    QJsonObject handleRecordFeedback(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetFrequency(uint64_t id, const QJsonObject& params);

    // ── M2 handlers ──
    QJsonObject handleRecordInteraction(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetPathPreferences(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetFileTypeAffinity(uint64_t id);
    QJsonObject handleRunAggregation(uint64_t id);
    QJsonObject handleExportInteractionData(uint64_t id, const QJsonObject& params);
    QJsonObject handleRebuildVectorIndex(uint64_t id, const QJsonObject& params);
    void runVectorRebuildWorker(uint64_t runId, QString dbPath, QString dataDir,
                                QString modelsDir,
                                QString indexPath, QString metaPath,
                                QString targetGeneration,
                                QStringList includePaths);

    // ── Store + services ──
    std::optional<SQLiteStore> m_store;
    TypoLexicon m_typoLexicon;
    Scorer m_scorer;

    // M2 modules — initialized lazily after store open
    std::unique_ptr<InteractionTracker> m_interactionTracker;
    std::unique_ptr<FeedbackAggregator> m_feedbackAggregator;
    std::unique_ptr<PathPreferences> m_pathPreferences;
    std::unique_ptr<TypeAffinity> m_typeAffinity;
    std::unique_ptr<ModelRegistry> m_modelRegistry;
    std::unique_ptr<EmbeddingManager> m_embeddingManager;
    std::unique_ptr<EmbeddingManager> m_fastEmbeddingManager;
    std::unique_ptr<VectorIndex> m_vectorIndex;
    std::unique_ptr<VectorIndex> m_fastVectorIndex;
    std::unique_ptr<VectorStore> m_vectorStore;
    std::unique_ptr<CrossEncoderReranker> m_crossEncoderReranker;
    std::unique_ptr<CrossEncoderReranker> m_fastCrossEncoderReranker;
    std::unique_ptr<PersonalizedLtr> m_personalizedLtr;
    std::unique_ptr<QaExtractiveModel> m_qaExtractiveModel;

    struct VectorRebuildState {
        enum class Status {
            Idle,
            Running,
            Succeeded,
            Failed,
        };

        Status status = Status::Idle;
        uint64_t runId = 0;
        QString startedAt;
        QString finishedAt;
        int totalCandidates = 0;
        int processed = 0;
        int embedded = 0;
        int skipped = 0;
        int failed = 0;
        int scopeCandidates = 0;
        QString lastError;
        QStringList scopeRoots;
    };

    static QString vectorRebuildStatusToString(VectorRebuildState::Status status);
    void updateVectorRebuildProgress(uint64_t runId, int totalCandidates,
                                     int processed, int embedded,
                                     int skipped, int failed);
    void joinVectorRebuildThread();

    mutable std::shared_mutex m_vectorIndexMutex;
    std::mutex m_vectorRebuildMutex;
    VectorRebuildState m_vectorRebuildState;
    std::thread m_vectorRebuildThread;
    std::atomic<bool> m_stopRebuildRequested{false};

    QString m_dataDir;
    QString m_dbPath;
    QString m_vectorIndexPath;
    QString m_vectorMetaPath;
    QString m_fastVectorIndexPath;
    QString m_fastVectorMetaPath;
    QString m_activeVectorGeneration = QStringLiteral("v1");
    QString m_targetVectorGeneration = QStringLiteral("v2");
    QString m_fastVectorGeneration = QStringLiteral("v3_fast");
    QString m_vectorMigrationState = QStringLiteral("idle");
    double m_vectorMigrationProgressPct = 0.0;
    QString m_activeVectorModelId = QStringLiteral("legacy");
    QString m_activeVectorProvider = QStringLiteral("cpu");
    int m_activeVectorDimensions = 384;

    QString vectorIndexPathForGeneration(const QString& generation) const;
    QString vectorMetaPathForGeneration(const QString& generation) const;
    void refreshVectorGenerationState();
    void maybeStartBackgroundVectorMigration();

    // Opens the store if not already open. Returns true on success.
    bool ensureStoreOpen();
    bool ensureM2ModulesInitialized();
    bool ensureTypoLexiconReady();

    // Initialize M2 modules after store is opened.
    void initM2Modules();
    void initBsignoreWatch();
    void reloadBsignore();
    bool isExcludedByBsignore(const QString& absolutePath) const;
    QJsonObject bsignoreStatusJson() const;
    QJsonObject processStatsForService(const QString& serviceName) const;
    QJsonObject queryStatsSnapshot() const;
    bool m_m2Initialized = false;
    bool m_typoLexiconBuildAttempted = false;
    bool m_typoLexiconReady = false;

    std::unique_ptr<QFileSystemWatcher> m_bsignoreWatcher;
    BsignoreParser m_bsignoreParser;
    QString m_bsignorePath;
    qint64 m_bsignoreLastLoadedAtMs = 0;
    int m_bsignorePatternCount = 0;
    bool m_bsignoreLoaded = false;

    std::atomic<uint64_t> m_searchCount{0};
    std::atomic<uint64_t> m_rewriteAppliedCount{0};
    std::atomic<uint64_t> m_semanticOnlyAdmittedCount{0};
    std::atomic<uint64_t> m_semanticOnlySuppressedCount{0};

    QueryCache m_queryCache;
};

} // namespace bs
