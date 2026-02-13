#include "core/learning/coreml_ranker.h"

namespace bs {

struct CoreMlRanker::Impl {
    QString modelRootDir;
};

CoreMlRanker::CoreMlRanker(QString modelRootDir)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->modelRootDir = std::move(modelRootDir);
}

CoreMlRanker::~CoreMlRanker() = default;

bool CoreMlRanker::initialize(QString* errorOut)
{
    if (errorOut) {
        *errorOut = QStringLiteral("coreml_not_supported_on_this_platform");
    }
    return false;
}

bool CoreMlRanker::hasModel() const
{
    return false;
}

bool CoreMlRanker::isUpdatable() const
{
    return false;
}

QString CoreMlRanker::modelVersion() const
{
    return QString();
}

int CoreMlRanker::featureDim() const
{
    return 0;
}

double CoreMlRanker::score(const QVector<double>& features, bool* okOut) const
{
    Q_UNUSED(features);
    if (okOut) {
        *okOut = false;
    }
    return 0.5;
}

double CoreMlRanker::boost(const QVector<double>& features, double blendAlpha, bool* okOut) const
{
    Q_UNUSED(features);
    Q_UNUSED(blendAlpha);
    if (okOut) {
        *okOut = false;
    }
    return 0.0;
}

bool CoreMlRanker::trainAndPromote(const QVector<TrainingExample>& samples,
                                   const OnlineRanker::TrainConfig& config,
                                   OnlineRanker::TrainMetrics* activeMetrics,
                                   OnlineRanker::TrainMetrics* candidateMetrics,
                                   QString* rejectReason)
{
    Q_UNUSED(samples);
    Q_UNUSED(config);
    if (activeMetrics) {
        activeMetrics->examples = 0;
        activeMetrics->logLoss = 0.0;
        activeMetrics->avgPredictionLatencyUs = 0.0;
        activeMetrics->predictionFailureRate = 0.0;
        activeMetrics->probabilitySaturationRate = 0.0;
    }
    if (candidateMetrics) {
        candidateMetrics->examples = 0;
        candidateMetrics->logLoss = 0.0;
        candidateMetrics->avgPredictionLatencyUs = 0.0;
        candidateMetrics->predictionFailureRate = 0.0;
        candidateMetrics->probabilitySaturationRate = 0.0;
    }
    if (rejectReason) {
        *rejectReason = QStringLiteral("coreml_not_supported_on_this_platform");
    }
    return false;
}

} // namespace bs
