#pragma once

#include "core/learning/behavior_types.h"
#include "core/learning/online_ranker.h"

#include <QString>
#include <QVector>

#include <memory>

namespace bs {

class CoreMlRanker {
public:
    explicit CoreMlRanker(QString modelRootDir);
    ~CoreMlRanker();

    bool initialize(QString* errorOut = nullptr);

    bool hasModel() const;
    bool isUpdatable() const;
    QString modelVersion() const;
    int featureDim() const;

    double score(const QVector<double>& features, bool* okOut = nullptr) const;
    double boost(const QVector<double>& features, double blendAlpha, bool* okOut = nullptr) const;

    bool trainAndPromote(const QVector<TrainingExample>& samples,
                         const OnlineRanker::TrainConfig& config,
                         OnlineRanker::TrainMetrics* activeMetrics,
                         OnlineRanker::TrainMetrics* candidateMetrics,
                         QString* rejectReason);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bs

