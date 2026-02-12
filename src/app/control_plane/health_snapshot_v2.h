#pragma once

#include "app/control_plane/control_plane_types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>

namespace bs {

struct HealthSnapshotV2 {
    int schemaVersion = 2;
    QString snapshotId;
    qint64 snapshotTimeMs = 0;
    qint64 stalenessMs = 0;
    QString instanceId;

    QString overallState = QStringLiteral("unavailable");
    QString overallReason = QStringLiteral("unavailable");

    QJsonObject components;
    QJsonObject queue;
    QJsonObject index;
    QJsonObject vector;
    QJsonObject inference;
    QJsonObject processes;
    QJsonArray errors;

    // Compatibility aliases consumed by existing SettingsPanel formatting code.
    QJsonObject compatibility;
};

QJsonObject toJson(const HealthSnapshotV2& snapshot);
QVariantMap toVariantMap(const HealthSnapshotV2& snapshot);
HealthSnapshotV2 unavailableSnapshot(const QString& instanceId,
                                     const QString& reason,
                                     const QJsonArray& managedServices = {});

} // namespace bs
