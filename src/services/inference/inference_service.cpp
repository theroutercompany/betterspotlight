#include "inference_service.h"

#include "core/embedding/embedding_manager.h"
#include "core/models/model_manifest.h"
#include "core/models/model_registry.h"
#include "core/ranking/cross_encoder_reranker.h"
#include "core/ranking/qa_extractive_model.h"
#include "core/shared/logging.h"
#include "core/shared/search_result.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>

namespace bs {

namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString statusOr(const QString& value, const QString& fallback)
{
    return value.isEmpty() ? fallback : value;
}

bool envFlagEnabled(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on");
}

bool deterministicPlaceholderWorkersEnabled()
{
    return envFlagEnabled(qEnvironmentVariable("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"))
        || envFlagEnabled(qEnvironmentVariable("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"));
}

std::vector<QString> parseTextArray(const QJsonArray& array)
{
    std::vector<QString> out;
    out.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& value : array) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) {
            out.push_back(text);
        }
    }
    return out;
}

QJsonArray toJsonEmbedding(const std::vector<float>& embedding)
{
    QJsonArray out;
    for (float value : embedding) {
        out.append(static_cast<double>(value));
    }
    return out;
}

QJsonArray toJsonEmbeddings(const std::vector<std::vector<float>>& embeddings)
{
    QJsonArray out;
    for (const auto& emb : embeddings) {
        out.append(toJsonEmbedding(emb));
    }
    return out;
}

} // namespace

InferenceService::InferenceService(QObject* parent)
    : ServiceBase(QStringLiteral("inference"), parent)
{
    initWorkers();
}

InferenceService::~InferenceService()
{
    stopWorkers();
}

QJsonObject InferenceService::handleRequest(const QJsonObject& request)
{
    const QString method = request.value(QStringLiteral("method")).toString();
    const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("embed_query")) {
        return handleEmbedQuery(id, params);
    }
    if (method == QLatin1String("embed_passages")) {
        return handleEmbedPassages(id, params);
    }
    if (method == QLatin1String("rerank_fast")) {
        return handleRerank(id, params, Role::RerankFast);
    }
    if (method == QLatin1String("rerank_strong")) {
        return handleRerank(id, params, Role::RerankStrong);
    }
    if (method == QLatin1String("qa_extract")) {
        return handleQaExtract(id, params);
    }
    if (method == QLatin1String("cancel_request")) {
        return handleCancelRequest(id, params);
    }
    if (method == QLatin1String("get_inference_health")) {
        return handleGetInferenceHealth(id);
    }

    return ServiceBase::handleRequest(request);
}

void InferenceService::initWorkers()
{
    std::lock_guard<std::mutex> lock(m_workersMutex);
    m_workers.clear();

    const auto addWorker = [&](Role role) {
        auto worker = std::make_unique<Worker>();
        worker->role = role;
        worker->roleName = roleToString(role);
        worker->registry = std::make_unique<ModelRegistry>(ModelRegistry::resolveModelsDir());
        initializeWorkerModel(*worker);
        startWorkerThread(*worker);
        m_workers.push_back(std::move(worker));
    };

    addWorker(Role::EmbedStrong);
    addWorker(Role::EmbedFast);
    addWorker(Role::RerankFast);
    addWorker(Role::RerankStrong);
    addWorker(Role::QaExtractive);
    addWorker(Role::RebuildEmbedStrong);
    addWorker(Role::RebuildEmbedFast);
}

void InferenceService::startWorkerThread(Worker& worker)
{
    worker.stop = false;
    worker.thread = std::thread([this, &worker]() {
        workerLoop(worker);
    });
}

void InferenceService::stopWorkers()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(m_workersMutex);
        for (auto& worker : m_workers) {
            {
                std::lock_guard<std::mutex> workerLock(worker->mutex);
                worker->stop = true;
            }
            worker->cv.notify_all();
        }
        threads.reserve(m_workers.size());
        for (auto& worker : m_workers) {
            if (worker->thread.joinable()) {
                threads.push_back(std::move(worker->thread));
            }
        }
    }

    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void InferenceService::workerLoop(Worker& worker)
{
    for (;;) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(worker.mutex);
            worker.cv.wait(lock, [&]() {
                return worker.stop || !worker.liveQueue.empty() || !worker.rebuildQueue.empty();
            });
            if (worker.stop && worker.liveQueue.empty() && worker.rebuildQueue.empty()) {
                return;
            }
            if (!worker.liveQueue.empty()) {
                task = worker.liveQueue.front();
                worker.liveQueue.pop_front();
            } else if (!worker.rebuildQueue.empty()) {
                task = worker.rebuildQueue.front();
                worker.rebuildQueue.pop_front();
            }
        }

        if (!task) {
            continue;
        }

        QElapsedTimer timer;
        timer.start();

        const QString cancelToken = task->envelope.cancelToken;
        if (isCancelled(cancelToken)) {
            worker.cancelled.fetch_add(1);
            task->promise.set_value(makeStatusPayload(
                QStringLiteral("cancelled"),
                worker.roleName,
                QString(),
                timer.elapsed(),
                {},
                QStringLiteral("cancel_token")));
            continue;
        }

        if (task->envelope.deadlineMs > 0 && nowMs() > task->envelope.deadlineMs) {
            worker.timedOut.fetch_add(1);
            task->promise.set_value(makeStatusPayload(
                QStringLiteral("timeout"),
                worker.roleName,
                QString(),
                timer.elapsed(),
                {},
                QStringLiteral("deadline_exceeded")));
            continue;
        }

        QJsonObject payload;
        QString status = QStringLiteral("ok");
        QString fallbackReason;
        QString modelId;

        const auto lookupModelId = [&]() -> QString {
            if (worker.embedding) {
                return worker.embedding->activeModelId();
            }
            if (!worker.registry) {
                return QString();
            }
            const ModelManifest& manifest = worker.registry->manifest();
            const auto it = manifest.models.find(worker.roleName.toStdString());
            if (it == manifest.models.end()) {
                return QString();
            }
            return it->second.modelId;
        };

        if (!worker.available || worker.degraded) {
            status = QStringLiteral("degraded");
            if (deterministicPlaceholderWorkersEnabled()) {
                fallbackReason = QStringLiteral("placeholder_worker");
            } else {
                fallbackReason = worker.available
                    ? QStringLiteral("actor_degraded")
                    : QStringLiteral("model_unavailable");
            }
        } else {
            try {
                if (task->method == QLatin1String("embed_query")) {
                    if (!worker.embedding || !worker.embedding->isAvailable()) {
                        status = QStringLiteral("degraded");
                        fallbackReason = QStringLiteral("embedding_unavailable");
                    } else {
                        const QString query = task->params.value(QStringLiteral("query")).toString();
                        const bool applyQueryPrefix =
                            task->params.value(QStringLiteral("applyQueryPrefix")).toBool(true);
                        const std::vector<float> embedding = applyQueryPrefix
                            ? worker.embedding->embedQuery(query)
                            : worker.embedding->embed(query);
                        if (embedding.empty()) {
                            status = QStringLiteral("degraded");
                            fallbackReason = QStringLiteral("embedding_empty");
                        } else {
                            payload[QStringLiteral("embedding")] = toJsonEmbedding(embedding);
                            modelId = worker.embedding->activeModelId();
                        }
                    }
                } else if (task->method == QLatin1String("embed_passages")) {
                    if (!worker.embedding || !worker.embedding->isAvailable()) {
                        status = QStringLiteral("degraded");
                        fallbackReason = QStringLiteral("embedding_unavailable");
                    } else {
                        const std::vector<QString> texts = parseTextArray(
                            task->params.value(QStringLiteral("texts")).toArray());
                        const bool normalize = task->params.value(QStringLiteral("normalize")).toBool(true);
                        const bool rebuildPriority =
                            task->envelope.priority.compare(QStringLiteral("rebuild"), Qt::CaseInsensitive) == 0;
                        int microBatchSize = task->params.value(QStringLiteral("microBatchSize")).toInt(
                            rebuildPriority ? 8 : 0);
                        if (microBatchSize < 0) {
                            microBatchSize = 0;
                        }

                        std::vector<std::vector<float>> embeddings;
                        embeddings.reserve(texts.size());
                        if (!texts.empty()) {
                            if (microBatchSize > 0 && static_cast<int>(texts.size()) > microBatchSize) {
                                for (size_t offset = 0; offset < texts.size(); offset += static_cast<size_t>(microBatchSize)) {
                                    const size_t end = std::min(texts.size(), offset + static_cast<size_t>(microBatchSize));
                                    std::vector<QString> batch;
                                    batch.reserve(end - offset);
                                    for (size_t i = offset; i < end; ++i) {
                                        batch.push_back(texts[i]);
                                    }
                                    auto batchEmbeddings = worker.embedding->embedBatch(batch);
                                    if (batchEmbeddings.size() != batch.size()) {
                                        batchEmbeddings.clear();
                                        for (const QString& text : batch) {
                                            batchEmbeddings.push_back(worker.embedding->embed(text));
                                        }
                                    }
                                    embeddings.insert(embeddings.end(),
                                                      std::make_move_iterator(batchEmbeddings.begin()),
                                                      std::make_move_iterator(batchEmbeddings.end()));
                                }
                            } else {
                                embeddings = worker.embedding->embedBatch(texts);
                                if (embeddings.size() != texts.size()) {
                                    embeddings.clear();
                                    embeddings.reserve(texts.size());
                                    for (const QString& text : texts) {
                                        embeddings.push_back(worker.embedding->embed(text));
                                    }
                                }
                            }
                        }

                        if (embeddings.size() != texts.size()) {
                            status = QStringLiteral("degraded");
                            fallbackReason = QStringLiteral("embedding_size_mismatch");
                        } else {
                            if (normalize) {
                                for (auto& emb : embeddings) {
                                    double normSquared = 0.0;
                                    for (float value : emb) {
                                        normSquared += static_cast<double>(value) * static_cast<double>(value);
                                    }
                                    const double norm = std::sqrt(normSquared);
                                    if (norm > 0.0) {
                                        for (float& value : emb) {
                                            value = static_cast<float>(static_cast<double>(value) / norm);
                                        }
                                    }
                                }
                            }
                            payload[QStringLiteral("embeddings")] = toJsonEmbeddings(embeddings);
                            modelId = worker.embedding->activeModelId();
                        }
                    }
                } else if (task->method == QLatin1String("rerank_fast")
                           || task->method == QLatin1String("rerank_strong")) {
                    if (!worker.reranker || !worker.reranker->isAvailable()) {
                        status = QStringLiteral("degraded");
                        fallbackReason = QStringLiteral("reranker_unavailable");
                    } else {
                        const QString query = task->params.value(QStringLiteral("query")).toString();
                        const QJsonArray candidates = task->params.value(QStringLiteral("candidates")).toArray();

                        std::vector<SearchResult> results;
                        results.reserve(static_cast<size_t>(candidates.size()));
                        for (const QJsonValue& candidateValue : candidates) {
                            const QJsonObject candidate = candidateValue.toObject();
                            SearchResult result;
                            result.itemId = candidate.value(QStringLiteral("itemId")).toInteger();
                            result.path = candidate.value(QStringLiteral("path")).toString();
                            result.name = candidate.value(QStringLiteral("name")).toString();
                            result.snippet = candidate.value(QStringLiteral("snippet")).toString();
                            result.score = candidate.value(QStringLiteral("score")).toDouble();
                            results.push_back(std::move(result));
                        }

                        RerankerConfig config;
                        config.weight = 0.0f;
                        config.maxCandidates = static_cast<int>(results.size());
                        config.minScoreThreshold = 0.0f;
                        worker.reranker->rerank(query, results, config);

                        QJsonArray scores;
                        for (const SearchResult& result : results) {
                            QJsonObject score;
                            score[QStringLiteral("itemId")] = static_cast<qint64>(result.itemId);
                            score[QStringLiteral("score")] = static_cast<double>(result.crossEncoderScore);
                            scores.append(score);
                        }
                        payload[QStringLiteral("scores")] = scores;
                        modelId = lookupModelId();
                    }
                } else if (task->method == QLatin1String("qa_extract")) {
                    if (!worker.qa || !worker.qa->isAvailable()) {
                        status = QStringLiteral("degraded");
                        fallbackReason = QStringLiteral("qa_unavailable");
                    } else {
                        const QString query = task->params.value(QStringLiteral("query")).toString();
                        const int maxAnswerChars = std::clamp(
                            task->params.value(QStringLiteral("maxAnswerChars")).toInt(240),
                            80,
                            600);
                        const QJsonArray contexts = task->params.value(QStringLiteral("contexts")).toArray();

                        QaExtractiveModel::Answer best;
                        int bestContextIndex = -1;
                        for (int i = 0; i < contexts.size(); ++i) {
                            if (task->envelope.deadlineMs > 0 && nowMs() > task->envelope.deadlineMs) {
                                status = QStringLiteral("timeout");
                                fallbackReason = QStringLiteral("deadline_exceeded");
                                break;
                            }
                            const QString context = contexts.at(i).toString();
                            const auto answer = worker.qa->extract(query, context, maxAnswerChars);
                            if (answer.available && (!best.available || answer.confidence > best.confidence)) {
                                best = answer;
                                bestContextIndex = i;
                            }
                        }

                        if (status == QLatin1String("ok")) {
                            payload[QStringLiteral("available")] = best.available;
                            payload[QStringLiteral("answer")] = best.answer;
                            payload[QStringLiteral("confidence")] = best.confidence;
                            payload[QStringLiteral("rawScore")] = best.rawScore;
                            payload[QStringLiteral("startToken")] = best.startToken;
                            payload[QStringLiteral("endToken")] = best.endToken;
                            payload[QStringLiteral("contextIndex")] = bestContextIndex;
                            modelId = lookupModelId();
                        }
                    }
                } else {
                    status = QStringLiteral("error");
                    fallbackReason = QStringLiteral("unsupported_method");
                }
            } catch (const std::exception& ex) {
                status = QStringLiteral("error");
                fallbackReason = QString::fromUtf8(ex.what());
            } catch (...) {
                status = QStringLiteral("error");
                fallbackReason = QStringLiteral("unknown_exception");
            }
        }

        const bool placeholderResponse =
            (status == QLatin1String("degraded")
             && fallbackReason == QLatin1String("placeholder_worker"));

        if (status == QLatin1String("ok") || placeholderResponse) {
            worker.completed.fetch_add(1);
            worker.consecutiveFailures = 0;
            worker.degraded = placeholderResponse;
        } else if (status == QLatin1String("timeout")) {
            worker.timedOut.fetch_add(1);
            worker.consecutiveFailures = 0;
        } else if (status == QLatin1String("cancelled")) {
            worker.cancelled.fetch_add(1);
            worker.consecutiveFailures = 0;
        } else {
            worker.failed.fetch_add(1);
            worker.consecutiveFailures += 1;
            maybeRecoverWorker(worker);
        }

        task->promise.set_value(makeStatusPayload(
            status,
            worker.roleName,
            statusOr(modelId, lookupModelId()),
            timer.elapsed(),
            payload,
            fallbackReason));
    }
}

bool InferenceService::initializeWorkerModel(Worker& worker)
{
    const bool placeholderWorkers = deterministicPlaceholderWorkersEnabled();

    worker.registry.reset();
    worker.embedding.reset();
    worker.reranker.reset();
    worker.qa.reset();
    worker.available = false;
    worker.degraded = false;

    if (placeholderWorkers) {
        worker.available = true;
        worker.degraded = true;
        LOG_INFO(bsIpc,
                 "InferenceService: worker '%s' running in deterministic placeholder mode",
                 qUtf8Printable(worker.roleName));
        return true;
    }

    worker.registry = std::make_unique<ModelRegistry>(ModelRegistry::resolveModelsDir());

    const auto initializeEmbedding = [&](const char* role) {
        worker.embedding = std::make_unique<EmbeddingManager>(worker.registry.get(), role);
        return worker.embedding->initialize();
    };

    switch (worker.role) {
    case Role::EmbedStrong:
        worker.available = initializeEmbedding("bi-encoder");
        break;
    case Role::EmbedFast:
        worker.available = initializeEmbedding("bi-encoder-fast");
        break;
    case Role::RebuildEmbedStrong:
        worker.available = initializeEmbedding("bi-encoder");
        break;
    case Role::RebuildEmbedFast:
        worker.available = initializeEmbedding("bi-encoder-fast");
        break;
    case Role::RerankStrong:
        worker.reranker = std::make_unique<CrossEncoderReranker>(worker.registry.get(), "cross-encoder");
        worker.available = worker.reranker->initialize();
        break;
    case Role::RerankFast:
        worker.reranker = std::make_unique<CrossEncoderReranker>(worker.registry.get(), "cross-encoder-fast");
        worker.available = worker.reranker->initialize();
        break;
    case Role::QaExtractive:
        worker.qa = std::make_unique<QaExtractiveModel>(worker.registry.get(), "qa-extractive");
        worker.available = worker.qa->initialize();
        break;
    }

    if (worker.available) {
        worker.degraded = false;
        LOG_INFO(bsIpc, "InferenceService: worker '%s' initialized",
                 qUtf8Printable(worker.roleName));
    } else {
        LOG_WARN(bsIpc, "InferenceService: worker '%s' unavailable",
                 qUtf8Printable(worker.roleName));
    }

    return worker.available;
}

void InferenceService::maybeRecoverWorker(Worker& worker)
{
    if (worker.consecutiveFailures < kWorkerRestartThreshold) {
        return;
    }

    if (worker.restartAttempts >= kWorkerRestartBudget) {
        worker.degraded = true;
        LOG_WARN(bsIpc,
                 "InferenceService: worker '%s' degraded after %d restart attempts",
                 qUtf8Printable(worker.roleName),
                 worker.restartAttempts);
        return;
    }

    ++worker.restartAttempts;
    LOG_WARN(bsIpc,
             "InferenceService: recovering worker '%s' (attempt=%d)",
             qUtf8Printable(worker.roleName),
             worker.restartAttempts);

    initializeWorkerModel(worker);
    worker.consecutiveFailures = 0;
    if (!worker.available) {
        worker.degraded = true;
    }
}

QString InferenceService::roleToString(Role role)
{
    switch (role) {
    case Role::EmbedStrong:
        return QStringLiteral("bi-encoder");
    case Role::EmbedFast:
        return QStringLiteral("bi-encoder-fast");
    case Role::RerankStrong:
        return QStringLiteral("cross-encoder");
    case Role::RerankFast:
        return QStringLiteral("cross-encoder-fast");
    case Role::QaExtractive:
        return QStringLiteral("qa-extractive");
    case Role::RebuildEmbedStrong:
        return QStringLiteral("bi-encoder-rebuild");
    case Role::RebuildEmbedFast:
        return QStringLiteral("bi-encoder-fast-rebuild");
    }
    return QStringLiteral("unknown");
}

bool InferenceService::isRebuildRole(Role role)
{
    return role == Role::RebuildEmbedStrong || role == Role::RebuildEmbedFast;
}

bool InferenceService::isLiveRole(Role role)
{
    return !isRebuildRole(role);
}

QJsonObject InferenceService::handleEmbedQuery(uint64_t id, const QJsonObject& params)
{
    const QString role = params.value(QStringLiteral("role")).toString(QStringLiteral("bi-encoder"));
    Role workerRole = Role::EmbedStrong;
    if (role == QLatin1String("bi-encoder-fast")) {
        workerRole = Role::EmbedFast;
    }

    const RequestEnvelope envelope = parseEnvelope(params);
    const qint64 timeLeftMs = envelope.deadlineMs > 0
        ? std::max<qint64>(1, envelope.deadlineMs - nowMs())
        : static_cast<qint64>(200);

    auto payload = dispatch(workerRole, QStringLiteral("embed_query"), envelope, params,
                            static_cast<int>(std::min<qint64>(timeLeftMs + 25, 2000)));
    if (!payload.has_value()) {
        return IpcMessage::makeError(id,
                                     IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Inference worker unavailable"));
    }
    return IpcMessage::makeResponse(id, payload.value());
}

QJsonObject InferenceService::handleEmbedPassages(uint64_t id, const QJsonObject& params)
{
    const QString role = params.value(QStringLiteral("role")).toString(QStringLiteral("bi-encoder"));
    const QString priority = params.value(QStringLiteral("priority")).toString(QStringLiteral("live"));

    Role workerRole = Role::EmbedStrong;
    if (role == QLatin1String("bi-encoder-fast")) {
        workerRole = (priority.compare(QStringLiteral("rebuild"), Qt::CaseInsensitive) == 0)
            ? Role::RebuildEmbedFast
            : Role::EmbedFast;
    } else {
        workerRole = (priority.compare(QStringLiteral("rebuild"), Qt::CaseInsensitive) == 0)
            ? Role::RebuildEmbedStrong
            : Role::EmbedStrong;
    }

    const RequestEnvelope envelope = parseEnvelope(params);
    const qint64 timeLeftMs = envelope.deadlineMs > 0
        ? std::max<qint64>(1, envelope.deadlineMs - nowMs())
        : static_cast<qint64>(
            priority.compare(QStringLiteral("rebuild"), Qt::CaseInsensitive) == 0
                ? 6000
                : 600);
    auto payload = dispatch(workerRole, QStringLiteral("embed_passages"), envelope, params,
                            static_cast<int>(std::min<qint64>(timeLeftMs + 25, 10000)));
    if (!payload.has_value()) {
        return IpcMessage::makeError(id,
                                     IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Inference worker unavailable"));
    }
    return IpcMessage::makeResponse(id, payload.value());
}

QJsonObject InferenceService::handleRerank(uint64_t id,
                                           const QJsonObject& params,
                                           Role role)
{
    const RequestEnvelope envelope = parseEnvelope(params);
    const qint64 timeLeftMs = envelope.deadlineMs > 0
        ? std::max<qint64>(1, envelope.deadlineMs - nowMs())
        : static_cast<qint64>(500);
    const QString method = (role == Role::RerankFast)
        ? QStringLiteral("rerank_fast")
        : QStringLiteral("rerank_strong");

    auto payload = dispatch(role, method, envelope, params,
                            static_cast<int>(std::min<qint64>(timeLeftMs + 25, 2000)));
    if (!payload.has_value()) {
        return IpcMessage::makeError(id,
                                     IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Inference worker unavailable"));
    }
    return IpcMessage::makeResponse(id, payload.value());
}

QJsonObject InferenceService::handleQaExtract(uint64_t id, const QJsonObject& params)
{
    const RequestEnvelope envelope = parseEnvelope(params);
    const qint64 timeLeftMs = envelope.deadlineMs > 0
        ? std::max<qint64>(1, envelope.deadlineMs - nowMs())
        : static_cast<qint64>(1200);

    auto payload = dispatch(Role::QaExtractive,
                            QStringLiteral("qa_extract"),
                            envelope,
                            params,
                            static_cast<int>(std::min<qint64>(timeLeftMs + 25, 3000)));
    if (!payload.has_value()) {
        return IpcMessage::makeError(id,
                                     IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Inference worker unavailable"));
    }
    return IpcMessage::makeResponse(id, payload.value());
}

QJsonObject InferenceService::handleCancelRequest(uint64_t id, const QJsonObject& params)
{
    const QString cancelToken = params.value(QStringLiteral("cancelToken")).toString().trimmed();
    if (cancelToken.isEmpty()) {
        return IpcMessage::makeError(id,
                                     IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing cancelToken"));
    }

    markCancelled(cancelToken);
    garbageCollectCancelledTokens();

    QJsonObject result;
    result[QStringLiteral("cancelled")] = true;
    result[QStringLiteral("cancelToken")] = cancelToken;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject InferenceService::handleGetInferenceHealth(uint64_t id)
{
    QJsonObject result;
    result[QStringLiteral("connected")] = true;

    QJsonObject roleStatus;
    QJsonObject queueDepth;
    QJsonObject timeoutCounts;
    QJsonObject failureCounts;
    QJsonObject restartCounts;

    {
        std::lock_guard<std::mutex> lock(m_workersMutex);
        for (const auto& worker : m_workers) {
            const QString role = worker->roleName;
            roleStatus[role] = worker->degraded
                ? QStringLiteral("degraded")
                : (worker->available ? QStringLiteral("ready")
                                     : QStringLiteral("unavailable"));

            QJsonObject depth;
            {
                std::lock_guard<std::mutex> workerLock(worker->mutex);
                depth[QStringLiteral("live")] = static_cast<qint64>(worker->liveQueue.size());
                depth[QStringLiteral("rebuild")] = static_cast<qint64>(worker->rebuildQueue.size());
            }
            queueDepth[role] = depth;

            timeoutCounts[role] = worker->timedOut.load();
            failureCounts[role] = worker->failed.load();
            restartCounts[role] = worker->restartAttempts;
        }
    }

    result[QStringLiteral("roleStatusByModel")] = roleStatus;
    result[QStringLiteral("queueDepthByRole")] = queueDepth;
    result[QStringLiteral("timeoutCountByRole")] = timeoutCounts;
    result[QStringLiteral("failureCountByRole")] = failureCounts;
    result[QStringLiteral("restartCountByRole")] = restartCounts;

    return IpcMessage::makeResponse(id, result);
}

std::optional<QJsonObject> InferenceService::dispatch(Role role,
                                                      const QString& method,
                                                      const RequestEnvelope& envelope,
                                                      const QJsonObject& params,
                                                      int waitTimeoutMs)
{
    Worker* worker = workerForRole(role);
    const Worker* workerConst = static_cast<const InferenceService*>(this)->workerForRole(role);
    if (!worker || !workerConst) {
        return std::nullopt;
    }

    auto task = std::make_shared<Task>();
    task->method = method;
    task->envelope = envelope;
    task->params = params;

    std::future<QJsonObject> future = task->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        const bool rebuildRole = isRebuildRole(role);
        const bool liveRole = isLiveRole(role);
        auto& queue = rebuildRole ? worker->rebuildQueue : worker->liveQueue;
        const int queueLimit = rebuildRole ? kWorkerQueueLimitRebuild
                                           : (liveRole ? kWorkerQueueLimitLive
                                                       : kWorkerQueueLimitRebuild);
        if (static_cast<int>(queue.size()) >= queueLimit) {
            QJsonObject queueError = makeStatusPayload(
                QStringLiteral("degraded"),
                worker->roleName,
                QString(),
                /*elapsedMs=*/0,
                {},
                QStringLiteral("queue_full"));
            return queueError;
        }
        queue.push_back(task);
        worker->submitted.fetch_add(1);
    }
    worker->cv.notify_one();

    if (waitTimeoutMs <= 0) {
        waitTimeoutMs = 1;
    }

    const auto status = future.wait_for(std::chrono::milliseconds(waitTimeoutMs));
    if (status == std::future_status::ready) {
        return future.get();
    }

    if (!envelope.cancelToken.isEmpty()) {
        markCancelled(envelope.cancelToken);
    }
    worker->timedOut.fetch_add(1);

    return makeStatusPayload(
        QStringLiteral("timeout"),
        worker->roleName,
        QString(),
        waitTimeoutMs,
        {},
        QStringLiteral("rpc_timeout"));
}

InferenceService::RequestEnvelope InferenceService::parseEnvelope(const QJsonObject& params)
{
    RequestEnvelope envelope;
    envelope.requestId = params.value(QStringLiteral("requestId")).toString().trimmed();
    envelope.cancelToken = params.value(QStringLiteral("cancelToken")).toString().trimmed();
    envelope.traceId = params.value(QStringLiteral("traceId")).toString().trimmed();
    envelope.priority = params.value(QStringLiteral("priority"))
                            .toString(QStringLiteral("live"))
                            .trimmed()
                            .toLower();
    envelope.deadlineMs = params.value(QStringLiteral("deadlineMs")).toInteger(0);

    if (envelope.requestId.isEmpty()) {
        envelope.requestId = QString::number(nowMs());
    }
    if (envelope.cancelToken.isEmpty()) {
        envelope.cancelToken = envelope.requestId;
    }
    if (envelope.priority != QLatin1String("rebuild")) {
        envelope.priority = QStringLiteral("live");
    }
    return envelope;
}

QJsonObject InferenceService::makeStatusPayload(const QString& status,
                                                const QString& modelRole,
                                                const QString& modelId,
                                                qint64 elapsedMs,
                                                const QJsonObject& result,
                                                const QString& fallbackReason)
{
    QJsonObject payload;
    payload[QStringLiteral("status")] = status;
    payload[QStringLiteral("elapsedMs")] = elapsedMs;
    payload[QStringLiteral("modelRole")] = modelRole;
    payload[QStringLiteral("modelId")] = modelId;
    payload[QStringLiteral("result")] = result;
    payload[QStringLiteral("fallbackReason")] = fallbackReason;
    return payload;
}

bool InferenceService::isCancelled(const QString& cancelToken) const
{
    if (cancelToken.isEmpty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    return m_cancelledTokens.find(cancelToken.toStdString()) != m_cancelledTokens.end();
}

void InferenceService::markCancelled(const QString& cancelToken)
{
    if (cancelToken.isEmpty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    m_cancelledTokens.insert(cancelToken.toStdString());
}

void InferenceService::garbageCollectCancelledTokens()
{
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    if (m_cancelledTokens.size() > 4096) {
        m_cancelledTokens.clear();
    }
}

InferenceService::Worker* InferenceService::workerForRole(Role role)
{
    std::lock_guard<std::mutex> lock(m_workersMutex);
    for (auto& worker : m_workers) {
        if (worker->role == role) {
            return worker.get();
        }
    }
    return nullptr;
}

const InferenceService::Worker* InferenceService::workerForRole(Role role) const
{
    std::lock_guard<std::mutex> lock(m_workersMutex);
    for (const auto& worker : m_workers) {
        if (worker->role == role) {
            return worker.get();
        }
    }
    return nullptr;
}

} // namespace bs
