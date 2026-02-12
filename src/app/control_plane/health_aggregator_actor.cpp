#include "app/control_plane/health_aggregator_actor.h"

#include "core/ipc/service_base.h"
#include "core/ipc/socket_client.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QPointer>

#include <algorithm>

namespace bs {

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kEventDebounceMs = 150;
constexpr int kQueryProbeTimeoutMs = 250;
constexpr int kIndexerProbeTimeoutMs = 250;
constexpr int kInferenceProbeTimeoutMs = 300;
constexpr int kExtractorProbeTimeoutMs = 200;
constexpr qint64 kComponentStaleThresholdMs = 6000;
constexpr qint64 kSnapshotStaleThresholdMs = 6000;

QJsonObject mergedIndexHealth(const QJsonObject& queryHealthResult)
{
    const QJsonObject indexHealth = queryHealthResult.value(QStringLiteral("indexHealth")).toObject();
    if (!indexHealth.isEmpty()) {
        return indexHealth;
    }
    return queryHealthResult;
}

QJsonArray capErrors(const QJsonArray& errors, int cap = 50)
{
    if (errors.size() <= cap) {
        return errors;
    }
    QJsonArray out;
    for (int i = 0; i < cap; ++i) {
        out.append(errors.at(i));
    }
    return out;
}

} // namespace

HealthAggregatorActor::HealthAggregatorActor(QObject* parent)
    : QObject(parent)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &HealthAggregatorActor::refreshNow);

    m_eventDebounceTimer.setSingleShot(true);
    m_eventDebounceTimer.setInterval(kEventDebounceMs);
    connect(&m_eventDebounceTimer, &QTimer::timeout, this, &HealthAggregatorActor::refreshNow);
}

HealthAggregatorActor::~HealthAggregatorActor()
{
    stop();
}

void HealthAggregatorActor::initialize(const QString& instanceId)
{
    m_instanceId = instanceId.trimmed();
    if (m_instanceId.isEmpty()) {
        m_instanceId = qEnvironmentVariable("BETTERSPOTLIGHT_INSTANCE_ID");
    }
}

void HealthAggregatorActor::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_pollTimer.start();
    m_eventDebounceTimer.start();
}

void HealthAggregatorActor::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_refreshPending = false;
    m_refreshInFlight = false;
    m_pollTimer.stop();
    m_eventDebounceTimer.stop();
    if (m_queryClient) {
        m_queryClient->disconnect();
    }
    if (m_indexerClient) {
        m_indexerClient->disconnect();
    }
    if (m_inferenceClient) {
        m_inferenceClient->disconnect();
    }
    if (m_extractorClient) {
        m_extractorClient->disconnect();
    }
}

void HealthAggregatorActor::triggerRefresh()
{
    if (!m_running) {
        return;
    }
    m_eventDebounceTimer.start();
}

void HealthAggregatorActor::setManagedServices(const QJsonArray& services)
{
    m_managedServices = services;
    triggerRefresh();
}

bool HealthAggregatorActor::isManagedServiceReady(const QString& serviceName) const
{
    for (const QJsonValue& value : m_managedServices) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("name")).toString() != serviceName) {
            continue;
        }
        return row.value(QStringLiteral("running")).toBool(false)
            && row.value(QStringLiteral("ready")).toBool(false);
    }
    return false;
}

QString HealthAggregatorActor::managedServiceState(const QString& serviceName) const
{
    for (const QJsonValue& value : m_managedServices) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("name")).toString() != serviceName) {
            continue;
        }
        return row.value(QStringLiteral("state")).toString();
    }
    return QStringLiteral("unavailable");
}

void HealthAggregatorActor::fetchQueryHealthAsync(std::function<void(QJsonObject)> callback)
{
    if (!callback) {
        return;
    }

    if (!m_queryClient) {
        m_queryClient = std::make_unique<SocketClient>();
    }
    if (!m_queryClient->isConnected()) {
        const QString socketPath = ServiceBase::socketPath(QStringLiteral("query"));
        if (!m_queryClient->connectToServer(socketPath, 120)) {
            callback({});
            return;
        }
    }

    QPointer<HealthAggregatorActor> self(this);
    m_queryClient->sendRequestAsync(
        QStringLiteral("getQueryHealthV3"),
        {},
        kQueryProbeTimeoutMs,
        [self, callback = std::move(callback)](const std::optional<QJsonObject>& response) mutable {
            if (!self) {
                return;
            }
            if (!response.has_value()) {
                callback({});
                return;
            }
            if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
                callback({});
                return;
            }
            callback(response->value(QStringLiteral("result")).toObject());
        });
}

void HealthAggregatorActor::fetchIndexerQueueAsync(std::function<void(QJsonObject)> callback)
{
    if (!callback) {
        return;
    }

    if (!isManagedServiceReady(QStringLiteral("indexer"))) {
        callback({});
        return;
    }

    if (!m_indexerClient) {
        m_indexerClient = std::make_unique<SocketClient>();
    }
    if (!m_indexerClient->isConnected()) {
        if (!m_indexerClient->connectToServer(ServiceBase::socketPath(QStringLiteral("indexer")), 120)) {
            callback({});
            return;
        }
    }

    QPointer<HealthAggregatorActor> self(this);
    m_indexerClient->sendRequestAsync(
        QStringLiteral("getQueueStatus"),
        {},
        kIndexerProbeTimeoutMs,
        [self, callback = std::move(callback)](const std::optional<QJsonObject>& response) mutable {
            if (!self) {
                return;
            }
            if (!response.has_value()) {
                callback({});
                return;
            }
            if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
                callback({});
                return;
            }
            callback(response->value(QStringLiteral("result")).toObject());
        });
}

void HealthAggregatorActor::fetchInferenceHealthAsync(std::function<void(QJsonObject)> callback)
{
    if (!callback) {
        return;
    }

    if (!isManagedServiceReady(QStringLiteral("inference"))) {
        callback({});
        return;
    }

    if (!m_inferenceClient) {
        m_inferenceClient = std::make_unique<SocketClient>();
    }
    if (!m_inferenceClient->isConnected()) {
        if (!m_inferenceClient->connectToServer(ServiceBase::socketPath(QStringLiteral("inference")), 120)) {
            callback({});
            return;
        }
    }

    QPointer<HealthAggregatorActor> self(this);
    m_inferenceClient->sendRequestAsync(
        QStringLiteral("get_inference_health"),
        {},
        kInferenceProbeTimeoutMs,
        [self, callback = std::move(callback)](const std::optional<QJsonObject>& response) mutable {
            if (!self) {
                return;
            }
            if (!response.has_value()) {
                callback({});
                return;
            }
            if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
                callback({});
                return;
            }
            callback(response->value(QStringLiteral("result")).toObject());
        });
}

void HealthAggregatorActor::fetchExtractorHealthAsync(std::function<void(QJsonObject)> callback)
{
    if (!callback) {
        return;
    }

    if (!isManagedServiceReady(QStringLiteral("extractor"))) {
        callback({});
        return;
    }

    if (!m_extractorClient) {
        m_extractorClient = std::make_unique<SocketClient>();
    }
    if (!m_extractorClient->isConnected()) {
        if (!m_extractorClient->connectToServer(ServiceBase::socketPath(QStringLiteral("extractor")), 120)) {
            callback({});
            return;
        }
    }

    QPointer<HealthAggregatorActor> self(this);
    m_extractorClient->sendRequestAsync(
        QStringLiteral("ping"),
        {},
        kExtractorProbeTimeoutMs,
        [self, callback = std::move(callback)](const std::optional<QJsonObject>& response) mutable {
            if (!self) {
                return;
            }
            if (!response.has_value()) {
                callback({});
                return;
            }
            if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
                callback({});
                return;
            }
            callback(response->value(QStringLiteral("result")).toObject());
        });
}

QString HealthAggregatorActor::computeOverallState(const QJsonArray& services,
                                                   const QJsonObject& mergedHealth,
                                                   qint64 stalenessMs,
                                                   QString* reason)
{
    bool missingRequired = false;
    bool degradedService = false;
    bool rebuilding = false;

    for (const QJsonValue& value : services) {
        const QJsonObject row = value.toObject();
        const QString name = row.value(QStringLiteral("name")).toString();
        const bool required = (name == QLatin1String("indexer")
                               || name == QLatin1String("query")
                               || name == QLatin1String("inference")
                               || name == QLatin1String("extractor"));
        const bool running = row.value(QStringLiteral("running")).toBool(false);
        const bool ready = row.value(QStringLiteral("ready")).toBool(false);
        const QString state = row.value(QStringLiteral("state")).toString();

        if (required && (!running || !ready)) {
            missingRequired = true;
        }
        if (state == QLatin1String("degraded")
            || state == QLatin1String("backoff")
            || state == QLatin1String("crashed")
            || state == QLatin1String("giving_up")) {
            degradedService = true;
        }
    }

    if (mergedHealth.value(QStringLiteral("queueRebuildRunning")).toBool(false)
        || mergedHealth.value(QStringLiteral("vectorRebuildStatus")).toString() == QLatin1String("running")
        || mergedHealth.value(QStringLiteral("overallStatus")).toString() == QLatin1String("rebuilding")) {
        rebuilding = true;
    }
    if (mergedHealth.value(QStringLiteral("inferenceProbeState")).toString()
            == QLatin1String("unavailable")
        || mergedHealth.value(QStringLiteral("extractorProbeState")).toString()
               == QLatin1String("unavailable")) {
        degradedService = true;
    }

    if (missingRequired) {
        if (reason) *reason = QStringLiteral("required_service_unavailable");
        return QStringLiteral("unavailable");
    }
    if (stalenessMs > kSnapshotStaleThresholdMs) {
        if (reason) *reason = QStringLiteral("snapshot_stale");
        return QStringLiteral("stale");
    }
    if (degradedService
        || mergedHealth.value(QStringLiteral("overallStatus")).toString() == QLatin1String("degraded")
        || mergedHealth.value(QStringLiteral("criticalFailures")).toInteger() > 0) {
        if (reason) *reason = QStringLiteral("component_degraded");
        return QStringLiteral("degraded");
    }
    if (rebuilding) {
        if (reason) *reason = QStringLiteral("rebuilding");
        return QStringLiteral("rebuilding");
    }
    if (reason) *reason = QStringLiteral("healthy");
    return QStringLiteral("healthy");
}

void HealthAggregatorActor::refreshNow()
{
    if (!m_running) {
        return;
    }
    if (m_refreshInFlight) {
        m_refreshPending = true;
        return;
    }

    m_refreshInFlight = true;
    QPointer<HealthAggregatorActor> self(this);
    fetchQueryHealthAsync([self](QJsonObject queryHealth) {
        if (!self) {
            return;
        }
        self->fetchIndexerQueueAsync([self, queryHealth = std::move(queryHealth)](QJsonObject indexerQueue) {
            if (!self) {
                return;
            }
            self->fetchInferenceHealthAsync(
                [self,
                 queryHealth = std::move(queryHealth),
                 indexerQueue = std::move(indexerQueue)](QJsonObject inferenceHealth) mutable {
                    if (!self) {
                        return;
                    }
                    self->fetchExtractorHealthAsync(
                        [self,
                         queryHealth = std::move(queryHealth),
                         indexerQueue = std::move(indexerQueue),
                         inferenceHealth = std::move(inferenceHealth)](QJsonObject extractorHealth) mutable {
                            if (!self) {
                                return;
                            }
                            self->buildAndPublishSnapshot(
                                queryHealth,
                                indexerQueue,
                                inferenceHealth,
                                extractorHealth);
                            self->m_refreshInFlight = false;
                            if (self->m_refreshPending) {
                                self->m_refreshPending = false;
                                QTimer::singleShot(0, self, &HealthAggregatorActor::refreshNow);
                            }
                        });
                });
        });
    });
}

void HealthAggregatorActor::buildAndPublishSnapshot(const QJsonObject& queryHealth,
                                                    const QJsonObject& indexerQueue,
                                                    const QJsonObject& inferenceHealth,
                                                    const QJsonObject& extractorHealth)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    HealthSnapshotV2 snapshot = unavailableSnapshot(
        m_instanceId,
        QStringLiteral("health_unavailable"),
        m_managedServices);

    QJsonObject processes = snapshot.processes;
    processes[QStringLiteral("runtimeDir")] = qEnvironmentVariable("BETTERSPOTLIGHT_RUNTIME_DIR");
    processes[QStringLiteral("orphanCount")] = 0;
    snapshot.processes = processes;

    QJsonObject mergedHealth;
    if (!queryHealth.isEmpty()) {
        mergedHealth = mergedIndexHealth(queryHealth);
        snapshot.compatibility = mergedHealth;
    }
    if (!inferenceHealth.isEmpty()) {
        mergedHealth[QStringLiteral("inferenceServiceConnected")] =
            inferenceHealth.value(QStringLiteral("connected")).toBool(true);
        mergedHealth[QStringLiteral("inferenceRoleStatusByModel")] =
            inferenceHealth.value(QStringLiteral("roleStatusByModel")).toObject();
        mergedHealth[QStringLiteral("inferenceQueueDepthByRole")] =
            inferenceHealth.value(QStringLiteral("queueDepthByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceTimeoutCountByRole")] =
            inferenceHealth.value(QStringLiteral("timeoutCountByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceServiceFailureCountByRole")] =
            inferenceHealth.value(QStringLiteral("failureCountByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceServiceRestartCountByRole")] =
            inferenceHealth.value(QStringLiteral("restartCountByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceSupervisorStateByRole")] =
            inferenceHealth.value(QStringLiteral("supervisorStateByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceBackoffMsByRole")] =
            inferenceHealth.value(QStringLiteral("backoffMsByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceRestartBudgetExhaustedByRole")] =
            inferenceHealth.value(QStringLiteral("restartBudgetExhaustedByRole")).toObject();
        mergedHealth[QStringLiteral("inferenceProbeState")] = QStringLiteral("fresh");
    } else if (!mergedHealth.contains(QStringLiteral("inferenceProbeState"))) {
        mergedHealth[QStringLiteral("inferenceProbeState")] = QStringLiteral("unavailable");
    }

    if (!extractorHealth.isEmpty()) {
        mergedHealth[QStringLiteral("extractorProbeState")] = QStringLiteral("fresh");
        mergedHealth[QStringLiteral("extractorLastPingMs")] =
            extractorHealth.value(QStringLiteral("timestamp")).toInteger(now);
    } else if (!mergedHealth.contains(QStringLiteral("extractorProbeState"))) {
        mergedHealth[QStringLiteral("extractorProbeState")] = QStringLiteral("unavailable");
    }

    if (!indexerQueue.isEmpty()) {
        mergedHealth[QStringLiteral("queuePending")] = indexerQueue.value(QStringLiteral("pending")).toInt();
        mergedHealth[QStringLiteral("queueInProgress")] = indexerQueue.value(QStringLiteral("processing")).toInt();
        mergedHealth[QStringLiteral("queuePreparing")] = indexerQueue.value(QStringLiteral("preparing")).toInt();
        mergedHealth[QStringLiteral("queueWriting")] = indexerQueue.value(QStringLiteral("writing")).toInt();
        mergedHealth[QStringLiteral("queueFailed")] = indexerQueue.value(QStringLiteral("failed")).toInt();
        mergedHealth[QStringLiteral("queueDropped")] = indexerQueue.value(QStringLiteral("dropped")).toInt();
        mergedHealth[QStringLiteral("queuePaused")] = indexerQueue.value(QStringLiteral("paused")).toBool(false);
        mergedHealth[QStringLiteral("queueRebuildRunning")] =
            indexerQueue.value(QStringLiteral("rebuildRunning")).toBool(false);
        mergedHealth[QStringLiteral("queueRebuildStatus")] =
            indexerQueue.value(QStringLiteral("rebuildStatus")).toString(QStringLiteral("idle"));
        const QJsonObject lastProgress =
            indexerQueue.value(QStringLiteral("lastProgressReport")).toObject();
        mergedHealth[QStringLiteral("queueScanned")] = lastProgress.value(QStringLiteral("scanned")).toInt();
        mergedHealth[QStringLiteral("queueTotal")] = lastProgress.value(QStringLiteral("total")).toInt();
        mergedHealth[QStringLiteral("queueProgressPct")] = mergedHealth.value(QStringLiteral("queueTotal")).toInt() > 0
            ? (100.0 * static_cast<double>(mergedHealth.value(QStringLiteral("queueScanned")).toInt())
               / static_cast<double>(mergedHealth.value(QStringLiteral("queueTotal")).toInt()))
            : 0.0;
        mergedHealth[QStringLiteral("queueCoalesced")] = indexerQueue.value(QStringLiteral("coalesced")).toInt();
        mergedHealth[QStringLiteral("queueStaleDropped")] = indexerQueue.value(QStringLiteral("staleDropped")).toInt();
        mergedHealth[QStringLiteral("queuePrepWorkers")] = indexerQueue.value(QStringLiteral("prepWorkers")).toInt();
        mergedHealth[QStringLiteral("queueWriterBatchDepth")] =
            indexerQueue.value(QStringLiteral("writerBatchDepth")).toInt();
        mergedHealth[QStringLiteral("queueSource")] = QStringLiteral("indexer_rpc");
        mergedHealth[QStringLiteral("pipelineBulkhead")] =
            indexerQueue.value(QStringLiteral("bulkhead")).toObject();
    } else if (!mergedHealth.contains(QStringLiteral("queueSource"))) {
        mergedHealth[QStringLiteral("queueSource")] = QStringLiteral("unavailable");
    }

    const qint64 payloadSnapshotTime =
        mergedHealth.value(QStringLiteral("snapshotTimeMs")).toInteger(now);
    snapshot.stalenessMs = std::max<qint64>(0, now - payloadSnapshotTime);
    snapshot.snapshotTimeMs = now;
    snapshot.snapshotId = QStringLiteral("%1:%2").arg(m_instanceId, QString::number(now));
    snapshot.instanceId = m_instanceId;

    QString overallReason;
    snapshot.overallState = computeOverallState(
        m_managedServices, mergedHealth, snapshot.stalenessMs, &overallReason);
    snapshot.overallReason = overallReason;

    QJsonObject components;
    for (const QJsonValue& value : m_managedServices) {
        const QJsonObject row = value.toObject();
        const QString serviceName = row.value(QStringLiteral("name")).toString();
        HealthComponentV2 component;
        component.state = row.value(QStringLiteral("state")).toString();
        if (component.state.isEmpty()) {
            component.state = QStringLiteral("unavailable");
        }
        const bool running = row.value(QStringLiteral("running")).toBool(false);
        const bool ready = row.value(QStringLiteral("ready")).toBool(false);
        component.reason = (running && ready)
            ? QStringLiteral("running")
            : QStringLiteral("not_ready");
        component.lastUpdatedMs = row.value(QStringLiteral("updatedAtMs")).toInteger(now);
        component.stalenessMs = std::max<qint64>(0, now - component.lastUpdatedMs);
        if (component.stalenessMs > kComponentStaleThresholdMs) {
            component.state = QStringLiteral("stale");
            component.reason = QStringLiteral("component_stale");
        } else if (serviceName == QLatin1String("inference")
                   && mergedHealth.value(QStringLiteral("inferenceProbeState")).toString()
                       != QLatin1String("fresh")) {
            if (component.state == QLatin1String("ready")
                || component.state == QLatin1String("running")) {
                component.state = QStringLiteral("degraded");
            }
            component.reason = QStringLiteral("probe_unavailable");
        } else if (serviceName == QLatin1String("extractor")
                   && mergedHealth.value(QStringLiteral("extractorProbeState")).toString()
                       != QLatin1String("fresh")) {
            if (component.state == QLatin1String("ready")
                || component.state == QLatin1String("running")) {
                component.state = QStringLiteral("degraded");
            }
            component.reason = QStringLiteral("probe_unavailable");
        }
        component.metrics = row;
        components[serviceName] = healthComponentToJson(component);
    }
    snapshot.components = components;

    QJsonObject queue;
    queue[QStringLiteral("pending")] = mergedHealth.value(QStringLiteral("queuePending")).toInt();
    queue[QStringLiteral("inProgress")] = mergedHealth.value(QStringLiteral("queueInProgress")).toInt();
    queue[QStringLiteral("preparing")] = mergedHealth.value(QStringLiteral("queuePreparing")).toInt();
    queue[QStringLiteral("writing")] = mergedHealth.value(QStringLiteral("queueWriting")).toInt();
    queue[QStringLiteral("failed")] = mergedHealth.value(QStringLiteral("queueFailed")).toInt();
    queue[QStringLiteral("dropped")] = mergedHealth.value(QStringLiteral("queueDropped")).toInt();
    queue[QStringLiteral("coalesced")] = mergedHealth.value(QStringLiteral("queueCoalesced")).toInt();
    queue[QStringLiteral("staleDropped")] = mergedHealth.value(QStringLiteral("queueStaleDropped")).toInt();
    queue[QStringLiteral("prepWorkers")] = mergedHealth.value(QStringLiteral("queuePrepWorkers")).toInt();
    queue[QStringLiteral("writerBatchDepth")] = mergedHealth.value(QStringLiteral("queueWriterBatchDepth")).toInt();
    int embeddingQueueDepth = mergedHealth.value(QStringLiteral("queueEmbedding")).toInt(-1);
    if (embeddingQueueDepth < 0) {
        const QJsonObject roleDepth = mergedHealth.value(QStringLiteral("inferenceQueueDepthByRole")).toObject();
        int sum = 0;
        for (auto it = roleDepth.begin(); it != roleDepth.end(); ++it) {
            if (!it.key().startsWith(QStringLiteral("bi-encoder"))) {
                continue;
            }
            const QJsonObject depth = it.value().toObject();
            sum += depth.value(QStringLiteral("live")).toInt();
            sum += depth.value(QStringLiteral("rebuild")).toInt();
        }
        embeddingQueueDepth = sum;
    }
    queue[QStringLiteral("embeddingQueue")] = embeddingQueueDepth;
    queue[QStringLiteral("source")] = mergedHealth.value(QStringLiteral("queueSource")).toString();
    const QJsonObject bulkhead = mergedHealth.value(QStringLiteral("pipelineBulkhead")).toObject();
    if (!bulkhead.isEmpty()) {
        queue[QStringLiteral("bulkhead")] = bulkhead;
    }
    snapshot.queue = queue;

    QJsonObject index;
    index[QStringLiteral("files")] = mergedHealth.value(QStringLiteral("totalIndexedItems")).toInteger();
    index[QStringLiteral("chunks")] = mergedHealth.value(QStringLiteral("totalChunks")).toInteger();
    index[QStringLiteral("coverage")] = mergedHealth.value(QStringLiteral("contentCoveragePct")).toDouble();
    index[QStringLiteral("semanticCoverage")] = mergedHealth.value(QStringLiteral("semanticCoveragePct")).toDouble();
    index[QStringLiteral("dbSize")] = mergedHealth.value(QStringLiteral("ftsIndexSize")).toInteger();
    index[QStringLiteral("vectorSize")] = mergedHealth.value(QStringLiteral("vectorIndexSize")).toInteger();
    snapshot.index = index;

    QJsonObject vector;
    vector[QStringLiteral("activeEmbedded")] = mergedHealth.value(QStringLiteral("totalEmbeddedVectors")).toInteger();
    vector[QStringLiteral("rebuildEmbedded")] = mergedHealth.value(QStringLiteral("vectorRebuildEmbedded")).toInteger();
    vector[QStringLiteral("rebuildStatus")] = mergedHealth.value(QStringLiteral("vectorRebuildStatus")).toString();
    snapshot.vector = vector;

    QJsonObject inference;
    inference[QStringLiteral("connected")] = mergedHealth.value(QStringLiteral("inferenceServiceConnected")).toBool(false);
    inference[QStringLiteral("roleStatusByModel")] =
        mergedHealth.value(QStringLiteral("inferenceRoleStatusByModel")).toObject();
    inference[QStringLiteral("queueDepthByRole")] =
        mergedHealth.value(QStringLiteral("inferenceQueueDepthByRole")).toObject();
    inference[QStringLiteral("timeoutCountByRole")] =
        mergedHealth.value(QStringLiteral("inferenceTimeoutCountByRole")).toObject();
    inference[QStringLiteral("failureCountByRole")] =
        mergedHealth.value(QStringLiteral("inferenceServiceFailureCountByRole")).toObject();
    inference[QStringLiteral("restartCountByRole")] =
        mergedHealth.value(QStringLiteral("inferenceServiceRestartCountByRole")).toObject();
    inference[QStringLiteral("supervisorStateByRole")] =
        mergedHealth.value(QStringLiteral("inferenceSupervisorStateByRole")).toObject();
    inference[QStringLiteral("backoffMsByRole")] =
        mergedHealth.value(QStringLiteral("inferenceBackoffMsByRole")).toObject();
    inference[QStringLiteral("restartBudgetExhaustedByRole")] =
        mergedHealth.value(QStringLiteral("inferenceRestartBudgetExhaustedByRole")).toObject();
    snapshot.inference = inference;

    QJsonObject compat = snapshot.compatibility;
    if (compat.isEmpty()) {
        compat = mergedHealth;
    }
    compat[QStringLiteral("supervisorServices")] = m_managedServices;
    snapshot.compatibility = compat;

    const QJsonArray recentErrors = mergedHealth.value(QStringLiteral("recentErrors")).toArray();
    const QJsonArray detailedErrors = mergedHealth.value(QStringLiteral("detailedFailures")).toArray();
    if (!detailedErrors.isEmpty()) {
        snapshot.errors = capErrors(detailedErrors);
    } else {
        snapshot.errors = capErrors(recentErrors);
    }

    m_lastSnapshotTimeMs = now;
    emit snapshotUpdated(toJson(snapshot));
}

} // namespace bs
