#include "core/indexing/pipeline_telemetry_actor.h"

namespace bs {

PipelineTelemetryActor::PipelineTelemetryActor(QObject* parent)
    : QObject(parent)
{
}

PipelineTelemetryActor::~PipelineTelemetryActor() = default;

void PipelineTelemetryActor::recordWriterDispatch(PipelineLane lane)
{
    if (lane == PipelineLane::Live) {
        m_writerDispatchLive.fetch_add(1);
    } else {
        m_writerDispatchRebuild.fetch_add(1);
    }
}

void PipelineTelemetryActor::recordDrop(PipelineLane lane, const QString& reason)
{
    if (lane == PipelineLane::Live) {
        m_dropLive.fetch_add(1);
    } else {
        m_dropRebuild.fetch_add(1);
    }

    const QString normalized = reason.trimmed().toLower();
    if (normalized == QLatin1String("memory_soft")) {
        m_dropMemorySoft.fetch_add(1);
    } else if (normalized == QLatin1String("memory_hard")) {
        m_dropMemoryHard.fetch_add(1);
    } else if (normalized == QLatin1String("writer_lag")) {
        m_dropWriterLag.fetch_add(1);
    } else {
        m_dropQueueFull.fetch_add(1);
    }
}

void PipelineTelemetryActor::recordCoalesced()
{
    m_coalesced.fetch_add(1);
}

void PipelineTelemetryActor::recordStaleDrop()
{
    m_staleDrop.fetch_add(1);
}

void PipelineTelemetryActor::recordPrepWorkers(size_t workers)
{
    m_prepWorkers.store(workers);
}

void PipelineTelemetryActor::recordWriterBatchDepth(size_t depth)
{
    m_writerBatchDepth.store(depth);
}

QJsonObject PipelineTelemetryActor::snapshot() const
{
    QJsonObject out;
    const qint64 writerLive = static_cast<qint64>(m_writerDispatchLive.load());
    const qint64 writerRebuild = static_cast<qint64>(m_writerDispatchRebuild.load());
    const qint64 writerTotal = writerLive + writerRebuild;

    out[QStringLiteral("writerDispatchLive")] = writerLive;
    out[QStringLiteral("writerDispatchRebuild")] = writerRebuild;
    out[QStringLiteral("writerDispatchTotal")] = writerTotal;
    out[QStringLiteral("writerLaneShareLive")] =
        writerTotal > 0 ? static_cast<double>(writerLive) / static_cast<double>(writerTotal) : 0.0;
    out[QStringLiteral("writerLaneShareRebuild")] =
        writerTotal > 0 ? static_cast<double>(writerRebuild) / static_cast<double>(writerTotal) : 0.0;

    out[QStringLiteral("dropLive")] = static_cast<qint64>(m_dropLive.load());
    out[QStringLiteral("dropRebuild")] = static_cast<qint64>(m_dropRebuild.load());
    out[QStringLiteral("dropQueueFull")] = static_cast<qint64>(m_dropQueueFull.load());
    out[QStringLiteral("dropMemorySoft")] = static_cast<qint64>(m_dropMemorySoft.load());
    out[QStringLiteral("dropMemoryHard")] = static_cast<qint64>(m_dropMemoryHard.load());
    out[QStringLiteral("dropWriterLag")] = static_cast<qint64>(m_dropWriterLag.load());
    out[QStringLiteral("coalesced")] = static_cast<qint64>(m_coalesced.load());
    out[QStringLiteral("staleDrop")] = static_cast<qint64>(m_staleDrop.load());
    out[QStringLiteral("prepWorkers")] = static_cast<qint64>(m_prepWorkers.load());
    out[QStringLiteral("writerBatchDepth")] = static_cast<qint64>(m_writerBatchDepth.load());
    return out;
}

void PipelineTelemetryActor::reset()
{
    m_writerDispatchLive.store(0);
    m_writerDispatchRebuild.store(0);
    m_dropLive.store(0);
    m_dropRebuild.store(0);
    m_dropQueueFull.store(0);
    m_dropMemorySoft.store(0);
    m_dropMemoryHard.store(0);
    m_dropWriterLag.store(0);
    m_coalesced.store(0);
    m_staleDrop.store(0);
    m_prepWorkers.store(0);
    m_writerBatchDepth.store(0);
}

} // namespace bs
