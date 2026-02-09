#pragma once

#include "core/ipc/service_base.h"
#include "core/index/sqlite_store.h"
#include "core/index/typo_lexicon.h"
#include "core/fs/bsignore_parser.h"
#include "core/ranking/scorer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <QFileSystemWatcher>
#include <QStringList>
#include <shared_mutex>
#include <thread>

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
class SocketClient;

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
                                QString modelPath, QString vocabPath,
                                QString indexPath, QString metaPath,
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
    std::unique_ptr<EmbeddingManager> m_embeddingManager;
    std::unique_ptr<VectorIndex> m_vectorIndex;
    std::unique_ptr<VectorStore> m_vectorStore;
    std::unique_ptr<SocketClient> m_indexerClient;

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
    QString m_embeddingModelPath;
    QString m_embeddingVocabPath;

    // Opens the store if not already open. Returns true on success.
    bool ensureStoreOpen();

    // Initialize M2 modules after store is opened.
    void initM2Modules();
    void initBsignoreWatch();
    void reloadBsignore();
    bool isExcludedByBsignore(const QString& absolutePath) const;
    QJsonObject bsignoreStatusJson() const;
    QJsonObject processStatsForService(const QString& serviceName) const;
    QJsonObject queryStatsSnapshot() const;
    bool m_m2Initialized = false;

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
};

} // namespace bs
