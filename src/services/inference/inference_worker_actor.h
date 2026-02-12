#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>

namespace bs {

class InferenceWorkerActor : public QObject {
    Q_OBJECT
public:
    struct AdmissionDecision {
        bool accepted = false;
        QString reason;
        int laneQueueDepth = 0;
        int laneQueueLimit = 0;
        int globalLaneDepth = 0;
        int globalLaneLimit = 0;
    };

    explicit InferenceWorkerActor(QObject* parent = nullptr);
    ~InferenceWorkerActor() override;

    static AdmissionDecision admitLive(int workerLiveDepth,
                                       int workerLiveLimit,
                                       int globalLiveDepth,
                                       int globalLiveLimit);
    static AdmissionDecision admitRebuild(int workerRebuildDepth,
                                          int workerRebuildLimit,
                                          int globalRebuildDepth,
                                          int globalRebuildLimit);
    static QJsonObject toJson(const AdmissionDecision& decision);
};

} // namespace bs
