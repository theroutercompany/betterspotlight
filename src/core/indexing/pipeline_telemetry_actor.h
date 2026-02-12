#pragma once

#include "core/indexing/pipeline_scheduler_actor.h"

#include <QObject>
#include <QJsonObject>
#include <QString>

#include <atomic>

namespace bs {

class PipelineTelemetryActor : public QObject {
    Q_OBJECT
public:
    explicit PipelineTelemetryActor(QObject* parent = nullptr);
    ~PipelineTelemetryActor() override;

    void recordWriterDispatch(PipelineLane lane);
    void recordDrop(PipelineLane lane, const QString& reason);
    void recordCoalesced();
    void recordStaleDrop();
    void recordPrepWorkers(size_t workers);
    void recordWriterBatchDepth(size_t depth);

    QJsonObject snapshot() const;
    void reset();

private:
    std::atomic<size_t> m_writerDispatchLive{0};
    std::atomic<size_t> m_writerDispatchRebuild{0};
    std::atomic<size_t> m_dropLive{0};
    std::atomic<size_t> m_dropRebuild{0};
    std::atomic<size_t> m_dropQueueFull{0};
    std::atomic<size_t> m_dropMemorySoft{0};
    std::atomic<size_t> m_dropMemoryHard{0};
    std::atomic<size_t> m_dropWriterLag{0};
    std::atomic<size_t> m_coalesced{0};
    std::atomic<size_t> m_staleDrop{0};
    std::atomic<size_t> m_prepWorkers{0};
    std::atomic<size_t> m_writerBatchDepth{0};
};

} // namespace bs
