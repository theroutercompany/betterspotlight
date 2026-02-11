#pragma once

#include "core/ipc/service_base.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace bs {

class EmbeddingManager;
class CrossEncoderReranker;
class QaExtractiveModel;
class ModelRegistry;

class InferenceService final : public ServiceBase {
    Q_OBJECT
public:
    explicit InferenceService(QObject* parent = nullptr);
    ~InferenceService() override;

protected:
    QJsonObject handleRequest(const QJsonObject& request) override;

private:
    enum class Role {
        EmbedStrong,
        EmbedFast,
        RerankStrong,
        RerankFast,
        QaExtractive,
        RebuildEmbedStrong,
        RebuildEmbedFast,
    };

    struct RequestEnvelope {
        QString requestId;
        QString cancelToken;
        QString traceId;
        QString priority;
        qint64 deadlineMs = 0;
    };

    struct Task {
        QString method;
        RequestEnvelope envelope;
        QJsonObject params;
        std::promise<QJsonObject> promise;
    };

    struct Worker {
        Role role;
        QString roleName;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::shared_ptr<Task>> liveQueue;
        std::deque<std::shared_ptr<Task>> rebuildQueue;
        bool stop = false;

        std::unique_ptr<ModelRegistry> registry;
        std::unique_ptr<EmbeddingManager> embedding;
        std::unique_ptr<CrossEncoderReranker> reranker;
        std::unique_ptr<QaExtractiveModel> qa;

        bool available = false;
        bool degraded = false;
        int consecutiveFailures = 0;
        int restartAttempts = 0;

        std::atomic<qint64> submitted{0};
        std::atomic<qint64> completed{0};
        std::atomic<qint64> timedOut{0};
        std::atomic<qint64> cancelled{0};
        std::atomic<qint64> failed{0};
    };

    void initWorkers();
    void startWorkerThread(Worker& worker);
    void stopWorkers();
    void workerLoop(Worker& worker);

    bool initializeWorkerModel(Worker& worker);
    void maybeRecoverWorker(Worker& worker);

    static QString roleToString(Role role);
    static bool isRebuildRole(Role role);
    static bool isLiveRole(Role role);

    QJsonObject handleEmbedQuery(uint64_t id, const QJsonObject& params);
    QJsonObject handleEmbedPassages(uint64_t id, const QJsonObject& params);
    QJsonObject handleRerank(uint64_t id, const QJsonObject& params, Role role);
    QJsonObject handleQaExtract(uint64_t id, const QJsonObject& params);
    QJsonObject handleCancelRequest(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetInferenceHealth(uint64_t id);

    std::optional<QJsonObject> dispatch(Role role,
                                        const QString& method,
                                        const RequestEnvelope& envelope,
                                        const QJsonObject& params,
                                        int waitTimeoutMs);

    static RequestEnvelope parseEnvelope(const QJsonObject& params);
    static QJsonObject makeStatusPayload(const QString& status,
                                         const QString& modelRole,
                                         const QString& modelId,
                                         qint64 elapsedMs,
                                         const QJsonObject& result = {},
                                         const QString& fallbackReason = QString());

    bool isCancelled(const QString& cancelToken) const;
    void markCancelled(const QString& cancelToken);
    void garbageCollectCancelledTokens();

    Worker* workerForRole(Role role);
    const Worker* workerForRole(Role role) const;

    mutable std::mutex m_workersMutex;
    std::vector<std::unique_ptr<Worker>> m_workers;

    mutable std::mutex m_cancelMutex;
    std::unordered_set<std::string> m_cancelledTokens;

    std::atomic<uint64_t> m_requestSeq{1};

    static constexpr int kWorkerQueueLimitLive = 64;
    static constexpr int kWorkerQueueLimitRebuild = 512;
    static constexpr int kWorkerRestartThreshold = 3;
    static constexpr int kWorkerRestartBudget = 4;
};

} // namespace bs
