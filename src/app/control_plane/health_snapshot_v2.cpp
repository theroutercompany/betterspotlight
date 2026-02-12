#include "app/control_plane/health_snapshot_v2.h"

#include <QDateTime>

namespace bs {

namespace {

QJsonObject makeOverall(const HealthSnapshotV2& snapshot)
{
    QJsonObject overall;
    overall[QStringLiteral("state")] = snapshot.overallState;
    overall[QStringLiteral("reason")] = snapshot.overallReason;
    return overall;
}

QJsonObject makeCompat(const HealthSnapshotV2& snapshot)
{
    QJsonObject compat = snapshot.compatibility;
    compat[QStringLiteral("snapshotVersion")] = snapshot.schemaVersion;
    compat[QStringLiteral("snapshotId")] = snapshot.snapshotId;
    compat[QStringLiteral("snapshotTimeMs")] = snapshot.snapshotTimeMs;
    compat[QStringLiteral("stalenessMs")] = snapshot.stalenessMs;
    compat[QStringLiteral("instanceId")] = snapshot.instanceId;
    compat[QStringLiteral("overallStatus")] = snapshot.overallState;
    compat[QStringLiteral("healthStatusReason")] = snapshot.overallReason;
    compat[QStringLiteral("snapshotState")] = snapshot.overallState;
    return compat;
}

} // namespace

QJsonObject toJson(const HealthSnapshotV2& snapshot)
{
    QJsonObject out;
    out[QStringLiteral("schemaVersion")] = snapshot.schemaVersion;
    out[QStringLiteral("snapshotId")] = snapshot.snapshotId;
    out[QStringLiteral("snapshotTimeMs")] = snapshot.snapshotTimeMs;
    out[QStringLiteral("stalenessMs")] = snapshot.stalenessMs;
    out[QStringLiteral("instanceId")] = snapshot.instanceId;
    out[QStringLiteral("overall")] = makeOverall(snapshot);
    out[QStringLiteral("overallStatus")] = snapshot.overallState;
    out[QStringLiteral("healthStatusReason")] = snapshot.overallReason;
    out[QStringLiteral("snapshotState")] = snapshot.overallState;
    out[QStringLiteral("components")] = snapshot.components;
    out[QStringLiteral("queue")] = snapshot.queue;
    out[QStringLiteral("index")] = snapshot.index;
    out[QStringLiteral("vector")] = snapshot.vector;
    out[QStringLiteral("inference")] = snapshot.inference;
    out[QStringLiteral("processes")] = snapshot.processes;
    out[QStringLiteral("errors")] = snapshot.errors;

    const QJsonObject compat = makeCompat(snapshot);
    for (auto it = compat.begin(); it != compat.end(); ++it) {
        out[it.key()] = it.value();
    }

    return out;
}

QVariantMap toVariantMap(const HealthSnapshotV2& snapshot)
{
    return toJson(snapshot).toVariantMap();
}

HealthSnapshotV2 unavailableSnapshot(const QString& instanceId,
                                     const QString& reason,
                                     const QJsonArray& managedServices)
{
    HealthSnapshotV2 snapshot;
    snapshot.instanceId = instanceId;
    snapshot.snapshotTimeMs = QDateTime::currentMSecsSinceEpoch();
    snapshot.snapshotId = QStringLiteral("%1:%2").arg(instanceId, QString::number(snapshot.snapshotTimeMs));
    snapshot.stalenessMs = 0;
    snapshot.overallState = QStringLiteral("unavailable");
    snapshot.overallReason = reason;

    QJsonObject processes;
    processes[QStringLiteral("managed")] = managedServices;
    processes[QStringLiteral("orphanCount")] = 0;
    snapshot.processes = processes;
    return snapshot;
}

} // namespace bs
