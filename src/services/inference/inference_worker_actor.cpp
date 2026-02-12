#include "inference_worker_actor.h"

namespace bs {

InferenceWorkerActor::InferenceWorkerActor(QObject* parent)
    : QObject(parent)
{
}

InferenceWorkerActor::~InferenceWorkerActor() = default;

InferenceWorkerActor::AdmissionDecision InferenceWorkerActor::admitLive(
    int workerLiveDepth,
    int workerLiveLimit,
    int globalLiveDepth,
    int globalLiveLimit)
{
    AdmissionDecision decision;
    decision.laneQueueDepth = workerLiveDepth;
    decision.laneQueueLimit = workerLiveLimit;
    decision.globalLaneDepth = globalLiveDepth;
    decision.globalLaneLimit = globalLiveLimit;

    if (workerLiveDepth >= workerLiveLimit) {
        decision.reason = QStringLiteral("worker_queue_full");
        return decision;
    }
    if (globalLiveDepth >= globalLiveLimit) {
        decision.reason = QStringLiteral("global_live_queue_full");
        return decision;
    }

    decision.accepted = true;
    decision.reason = QStringLiteral("ok");
    return decision;
}

InferenceWorkerActor::AdmissionDecision InferenceWorkerActor::admitRebuild(
    int workerRebuildDepth,
    int workerRebuildLimit,
    int globalRebuildDepth,
    int globalRebuildLimit)
{
    AdmissionDecision decision;
    decision.laneQueueDepth = workerRebuildDepth;
    decision.laneQueueLimit = workerRebuildLimit;
    decision.globalLaneDepth = globalRebuildDepth;
    decision.globalLaneLimit = globalRebuildLimit;

    if (workerRebuildDepth >= workerRebuildLimit) {
        decision.reason = QStringLiteral("worker_queue_full");
        return decision;
    }
    if (globalRebuildDepth >= globalRebuildLimit) {
        decision.reason = QStringLiteral("global_rebuild_queue_full");
        return decision;
    }

    decision.accepted = true;
    decision.reason = QStringLiteral("ok");
    return decision;
}

QJsonObject InferenceWorkerActor::toJson(const AdmissionDecision& decision)
{
    QJsonObject out;
    out[QStringLiteral("accepted")] = decision.accepted;
    out[QStringLiteral("reason")] = decision.reason;
    out[QStringLiteral("laneQueueDepth")] = decision.laneQueueDepth;
    out[QStringLiteral("laneQueueLimit")] = decision.laneQueueLimit;
    out[QStringLiteral("globalLaneDepth")] = decision.globalLaneDepth;
    out[QStringLiteral("globalLaneLimit")] = decision.globalLaneLimit;
    return out;
}

} // namespace bs
