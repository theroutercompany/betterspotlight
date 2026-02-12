#include "app/control_plane/health_aggregator_actor.h"

#include "core/ipc/service_base.h"
#include "core/ipc/socket_client.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QDir>

namespace bs {

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kEventDebounceMs = 150;
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
    m_pollTimer.stop();
    m_eventDebounceTimer.stop();
    if (m_queryClient) {
        m_queryClient->disconnect();
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

QJsonObject HealthAggregatorActor::fetchQueryHealth()
{
    if (!m_queryClient) {
        m_queryClient = std::make_unique<SocketClient>();
    }
    if (!m_queryClient->isConnected()) {
        const QString socketPath = ServiceBase::socketPath(QStringLiteral("query"));
        if (!m_queryClient->connectToServer(socketPath, 120)) {
            return {};
        }
    }

    auto response = m_queryClient->sendRequest(QStringLiteral("getQueryHealthV3"), {}, 250);
    if (!response) {
        response = m_queryClient->sendRequest(QStringLiteral("getHealthV2"), {}, 250);
    }
    if (!response) {
        return {};
    }
    if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
        return {};
    }
    return response->value(QStringLiteral("result")).toObject();
}

QJsonObject HealthAggregatorActor::fetchIndexerQueue()
{
    if (!isManagedServiceReady(QStringLiteral("indexer"))) {
        return {};
    }

    SocketClient indexerClient;
    if (!indexerClient.connectToServer(ServiceBase::socketPath(QStringLiteral("indexer")), 120)) {
        return {};
    }
    const auto response = indexerClient.sendRequest(QStringLiteral("getQueueStatus"), {}, 250);
    if (!response.has_value()) {
        return {};
    }
    if (response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
        return {};
    }
    return response->value(QStringLiteral("result")).toObject();
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    HealthSnapshotV2 snapshot = unavailableSnapshot(
        m_instanceId,
        QStringLiteral("health_unavailable"),
        m_managedServices);

    QJsonObject processes = snapshot.processes;
    processes[QStringLiteral("runtimeDir")] = qEnvironmentVariable("BETTERSPOTLIGHT_RUNTIME_DIR");
    processes[QStringLiteral("orphanCount")] = 0;
    snapshot.processes = processes;

    QJsonObject queryHealth = fetchQueryHealth();
    QJsonObject mergedHealth;
    if (!queryHealth.isEmpty()) {
        mergedHealth = mergedIndexHealth(queryHealth);
        snapshot.compatibility = mergedHealth;
    }

    // Refresh queue metrics from indexer directly, but keep query-local summary as baseline.
    const QJsonObject indexerQueue = fetchIndexerQueue();
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
        mergedHealth[QStringLiteral("queueCoalesced")] = indexerQueue.value(QStringLiteral("coalesced")).toInt();
        mergedHealth[QStringLiteral("queueStaleDropped")] = indexerQueue.value(QStringLiteral("staleDropped")).toInt();
        mergedHealth[QStringLiteral("queuePrepWorkers")] = indexerQueue.value(QStringLiteral("prepWorkers")).toInt();
        mergedHealth[QStringLiteral("queueWriterBatchDepth")] =
            indexerQueue.value(QStringLiteral("writerBatchDepth")).toInt();
        mergedHealth[QStringLiteral("queueSource")] = QStringLiteral("indexer_rpc");
    } else {
        if (!mergedHealth.contains(QStringLiteral("queueSource"))) {
            mergedHealth[QStringLiteral("queueSource")] = QStringLiteral("unavailable");
        }
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

    // Components
    QJsonObject components;
    for (const QJsonValue& value : m_managedServices) {
        const QJsonObject row = value.toObject();
        HealthComponentV2 component;
        component.state = row.value(QStringLiteral("state")).toString();
        component.reason = row.value(QStringLiteral("running")).toBool(false)
            ? QStringLiteral("running")
            : QStringLiteral("not_running");
        component.lastUpdatedMs = row.value(QStringLiteral("updatedAtMs")).toInteger(now);
        component.stalenessMs = std::max<qint64>(0, now - component.lastUpdatedMs);
        component.metrics = row;
        components[row.value(QStringLiteral("name")).toString()] = healthComponentToJson(component);
    }
    snapshot.components = components;

    // Structured sections
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
    queue[QStringLiteral("embeddingQueue")] =
        mergedHealth.value(QStringLiteral("queueEmbedding")).toInt(0);
    queue[QStringLiteral("source")] = mergedHealth.value(QStringLiteral("queueSource")).toString();
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
