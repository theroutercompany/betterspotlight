#include "query_service.h"

#include "core/feedback/feedback_aggregator.h"
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/path_preferences.h"
#include "core/feedback/type_affinity.h"
#include "core/ipc/message.h"

#include <QDateTime>
#include <QJsonArray>

namespace bs {

QJsonObject QueryService::handleRecordInteraction(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    QString query = params.value(QStringLiteral("query")).toString();
    if (query.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'query' parameter"));
    }

    int64_t selectedItemId = static_cast<int64_t>(
        params.value(QStringLiteral("selectedItemId")).toInteger());
    if (selectedItemId <= 0) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing or invalid 'selectedItemId'"));
    }

    InteractionTracker::Interaction interaction;
    interaction.query = query;
    interaction.selectedItemId = selectedItemId;
    interaction.selectedPath = params.value(QStringLiteral("selectedPath")).toString();
    interaction.matchType = params.value(QStringLiteral("matchType")).toString();
    interaction.resultPosition = params.value(QStringLiteral("resultPosition")).toInt(0);
    interaction.frontmostApp = params.value(QStringLiteral("frontmostApp")).toString();
    interaction.timestamp = QDateTime::currentDateTimeUtc();

    const bool ok = m_interactionTracker && m_interactionTracker->recordInteraction(interaction);
    if (!ok) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to record interaction"));
    }

    if (m_pathPreferences) {
        m_pathPreferences->invalidateCache();
    }
    if (m_typeAffinity) {
        m_typeAffinity->invalidateCache();
    }

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetPathPreferences(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    int limit = params.value(QStringLiteral("limit")).toInt(50);
    if (limit < 1) {
        limit = 1;
    } else if (limit > 200) {
        limit = 200;
    }

    if (!m_pathPreferences) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("PathPreferences not initialized"));
    }

    const QVector<PathPreferences::DirPreference> dirs = m_pathPreferences->getTopDirectories(limit);

    QJsonArray dirsArray;
    for (const auto& dir : dirs) {
        QJsonObject obj;
        obj[QStringLiteral("directory")] = dir.directory;
        obj[QStringLiteral("selectionCount")] = dir.selectionCount;
        obj[QStringLiteral("boost")] = dir.boost;
        dirsArray.append(obj);
    }

    QJsonObject result;
    result[QStringLiteral("directories")] = dirsArray;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetFileTypeAffinity(uint64_t id)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_typeAffinity) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("TypeAffinity not initialized"));
    }

    const auto stats = m_typeAffinity->getAffinityStats();

    QJsonObject result;
    result[QStringLiteral("codeOpens")] = stats.codeOpens;
    result[QStringLiteral("documentOpens")] = stats.documentOpens;
    result[QStringLiteral("mediaOpens")] = stats.mediaOpens;
    result[QStringLiteral("otherOpens")] = stats.otherOpens;
    result[QStringLiteral("primaryAffinity")] = stats.primaryAffinity;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleRunAggregation(uint64_t id)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_feedbackAggregator) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("FeedbackAggregator not initialized"));
    }

    const bool aggOk = m_feedbackAggregator->runAggregation();
    const bool cleanupOk = m_feedbackAggregator->cleanup();

    if (m_pathPreferences) {
        m_pathPreferences->invalidateCache();
    }
    if (m_typeAffinity) {
        m_typeAffinity->invalidateCache();
    }

    QJsonObject result;
    result[QStringLiteral("aggregated")] = aggOk;
    result[QStringLiteral("cleanedUp")] = cleanupOk;
    result[QStringLiteral("lastAggregation")] =
        m_feedbackAggregator->lastAggregationTime().toString(Qt::ISODate);
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleExportInteractionData(uint64_t id, const QJsonObject& params)
{
    Q_UNUSED(params);

    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!m_interactionTracker) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("InteractionTracker not initialized"));
    }

    const QJsonArray data = m_interactionTracker->exportData();

    QJsonObject result;
    result[QStringLiteral("interactions")] = data;
    result[QStringLiteral("count")] = data.size();
    return IpcMessage::makeResponse(id, result);
}

} // namespace bs
