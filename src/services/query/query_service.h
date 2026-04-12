#pragma once

#include "core/ipc/service_base.h"
#include "core/index/sqlite_store.h"
#include "core/index/typo_lexicon.h"
#include "core/fs/bsignore_parser.h"
#include "core/ranking/scorer.h"
#include "core/query/query_cache.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <QFileSystemWatcher>
#include <QHash>
#include <QTimer>
#include <QStringList>
#include <shared_mutex>
#include <functional>
#include <thread>

struct sqlite3;

namespace bs {

class CrossEncoderReranker;
class EmbeddingManager;
class EmbeddingPipeline;
class SocketClient;
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
class LearningEngine;

class QueryService : public ServiceBase {
    Q_OBJECT
public:
    explicit QueryService(QObject* parent = nullptr);
    ~QueryService() override;

protected:
    QJsonObject handleRequest(const QJsonObject& request) override;
    void handleRequestWithResponder(const QJsonObject& request,
                                    RequestResponder responder) override;

private:
    enum class InferenceLane {
        Live,
        Health,
        Rebuild,
    };

    enum class RequestExecutionLane {
        Default,
        Health,
    };

    struct LaneTask {
        QJsonObject request;
        RequestResponder responder;
    };

    struct RuntimeMirror {
        bool m2Initialized = false;
        QString modelsDirResolved;
        bool requiredModelInventoryReady = false;
        QString requiredModelInventoryReason = QStringLiteral("required_models_unavailable");
        QStringList requiredModelInventoryMissingRoles;
        QString activeVectorGeneration = QStringLiteral("v1");
        QString targetVectorGeneration = QStringLiteral("v2");
        QString fastVectorGeneration = QStringLiteral("v3_fast");
        QString vectorMigrationState = QStringLiteral("idle");
        double vectorMigrationProgressPct = 0.0;
        QString activeVectorModelId = QStringLiteral("legacy");
        QString activeVectorProvider = QStringLiteral("cpu");
        int activeVectorDimensions = 384;
        bool embeddingManagerAvailable = false;
        QString embeddingManagerActiveModelId;
        bool fastEmbeddingManagerAvailable = false;
        QString fastEmbeddingManagerActiveModelId;
        bool crossEncoderAvailable = false;
        bool fastCrossEncoderAvailable = false;
        bool qaExtractiveAvailable = false;
        QJsonObject bsignoreStatus;
    };

    struct LocalHealthSnapshotCache {
        QJsonObject indexHealth;
        qint64 snapshotTimeMs = 0;
        QString state = QStringLiteral("unavailable");
    };

    struct PeerProbeCache {
        QJsonObject payload;
        qint64 snapshotTimeMs = 0;
        QString state = QStringLiteral("unavailable");
        bool refreshInFlight = false;
    };

    struct VectorRebuildCutoverPayload {
        uint64_t runId = 0;
        QString targetGeneration;
        QString targetIndexPath;
        QString targetMetaPath;
        QString tempIndexPath;
        QString tempMetaPath;
        QString modelId;
        QString provider;
        int dimensions = 0;
        int totalCandidates = 0;
        int processed = 0;
        int embedded = 0;
        int skipped = 0;
        int failed = 0;
        int expectedPrimaryMappings = 0;
        bool hasFastIndex = false;
        QString fastGeneration;
        QString fastIndexPath;
        QString fastMetaPath;
        QString tempFastIndexPath;
        QString tempFastMetaPath;
        QString fastModelId;
        QString fastProvider;
        int fastDimensions = 0;
        int expectedFastMappings = 0;
    };

    // ── M1 handlers ──
    QJsonObject handleSearch(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetAnswerSnippet(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetHealth(uint64_t id);
    QJsonObject handleGetQueryHealthV3(uint64_t id);
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
    QJsonObject handleRecordBehaviorEvent(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetLearningHealth(uint64_t id);
    QJsonObject handleSetLearningConsent(uint64_t id, const QJsonObject& params);
    QJsonObject handleTriggerLearningCycle(uint64_t id, const QJsonObject& params);
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
    std::unique_ptr<LearningEngine> m_learningEngine;

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
    bool m_requiredModelInventoryReady = false;
    QString m_requiredModelInventoryReason = QStringLiteral("required_models_unavailable");
    QStringList m_requiredModelInventoryMissingRoles;

    QString vectorIndexPathForGeneration(const QString& generation) const;
    QString vectorMetaPathForGeneration(const QString& generation) const;
    void refreshVectorGenerationState();

    // Opens the store if not already open. Returns true on success.
    bool ensureStoreOpen();
    void resolveDataPathsIfNeeded();
    bool ensureM2ModulesInitialized();
    bool ensureTypoLexiconReady();
    bool ensureHealthDiagnosticsOpen();
    void closeHealthDiagnostics();
    void startRequestExecutionLanes();
    void stopRequestExecutionLanes();
    void enqueueDefaultControlTask(std::function<void()> task);
    void enqueueRequestTask(RequestExecutionLane lane,
                            const QJsonObject& request,
                            RequestResponder responder);
    void requestLaneLoop(RequestExecutionLane lane);
    static bool isHealthRequestMethod(const QString& method);
    static bool requiresM2InitializationMethod(const QString& method);
    bool ensureInferenceClientConnected(InferenceLane lane);
    std::optional<QJsonObject> sendInferenceRequest(InferenceLane lane,
                                                    const QString& method,
                                                    const QJsonObject& params,
                                                    int timeoutMs,
                                                    const QString& roleForMetrics,
                                                    const QString& fallbackReasonKey,
                                                    const QString& cancelToken = QString());
    void recordInferenceTimeout(const QString& role);
    void recordInferenceFallback(const QString& role);
    void recordInferenceConnected(InferenceLane lane, bool connected);
    void disconnectInferenceLane(InferenceLane lane);
    static QString inferenceLaneName(InferenceLane lane);
    QJsonObject inferenceHealthSnapshot();
    void schedulePeerProbeRefresh(bool force = false);
    void refreshPeerProbesIfNeeded(bool force = false);
    void refreshIndexerPeerProbe(bool force = false);
    void refreshInferencePeerProbe(bool force = false);
    RuntimeMirror runtimeMirrorSnapshot() const;
    void refreshRuntimeMirror();
    void maybeRefreshLocalHealthSnapshot();
    QJsonObject buildLocalHealthSnapshotFromDiagnostics();
    QJsonObject buildRuntimeSettingsSnapshot(sqlite3* db) const;
    QJsonArray buildModelManifestSnapshot(const QString& modelsDirResolved,
                                          const RuntimeMirror& runtimeMirror,
                                          const QJsonObject& runtimeSettings,
                                          const QJsonObject& inferenceHealth) const;
    void applyVectorRebuildCutover(const VectorRebuildCutoverPayload& payload);
    void noteLearningSchedulerOutcome(bool promoted, const QString& reason);
    QJsonObject learningSchedulerSnapshot() const;
    QJsonObject learningHealthSnapshot() const;

    // Initialize M2 modules after store is opened.
    void initM2Modules();
    void initBsignoreWatch();
    void reloadBsignore();
    bool isExcludedByBsignore(const QString& absolutePath) const;
    QJsonObject bsignoreStatusJson() const;
    QJsonObject processStatsForService(const QString& serviceName) const;
    QJsonObject queryStatsSnapshot() const;
    QJsonObject handleGetHealthInternal(uint64_t id, bool includeIndexerQueueProbe);
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

    std::unique_ptr<SocketClient> m_inferenceLiveClient;
    std::unique_ptr<SocketClient> m_inferenceHealthClient;
    std::unique_ptr<SocketClient> m_inferenceRebuildClient;
    mutable std::mutex m_inferenceRpcMutexLive;
    mutable std::mutex m_inferenceRpcMutexHealth;
    mutable std::mutex m_inferenceRpcMutexRebuild;
    mutable std::mutex m_inferenceStatsMutex;
    mutable std::mutex m_learningSchedulerMutex;
    QTimer m_learningSchedulerTimer;
    QTimer m_peerProbeRefreshTimer;
    qint64 m_learningSchedulerLastTickAtMs = 0;
    qint64 m_learningSchedulerLastPromotedAtMs = 0;
    qint64 m_learningSchedulerTicks = 0;
    qint64 m_learningSchedulerPromoted = 0;
    QString m_learningSchedulerLastReason;
    QHash<QString, qint64> m_learningSchedulerReasonCounts;
    std::mutex m_requestLaneMutex;
    std::condition_variable m_requestLaneCv;
    std::deque<LaneTask> m_defaultRequestQueue;
    std::deque<LaneTask> m_healthRequestQueue;
    std::deque<std::function<void()>> m_defaultControlQueue;
    std::thread m_defaultRequestThread;
    std::thread m_healthRequestThread;
    bool m_stopRequestLanes = false;
    std::atomic<bool> m_shuttingDown{false};
    sqlite3* m_healthDiagnosticsDb = nullptr;
    mutable std::mutex m_runtimeMirrorMutex;
    RuntimeMirror m_runtimeMirror;
    mutable std::mutex m_peerProbeMutex;
    PeerProbeCache m_indexerPeerCache;
    PeerProbeCache m_inferencePeerCache;
    LocalHealthSnapshotCache m_localHealthSnapshotCache;
    std::unique_ptr<SocketClient> m_indexerHealthClient;
    bool m_inferenceServiceConnected = false;
    bool m_inferenceLiveLaneConnected = false;
    bool m_inferenceHealthLaneConnected = false;
    bool m_inferenceRebuildLaneConnected = false;
    QHash<QString, qint64> m_inferenceTimeoutCountByRole;
    QHash<QString, qint64> m_inferenceFallbackCountByRole;

    QueryCache m_queryCache;
};

} // namespace bs
