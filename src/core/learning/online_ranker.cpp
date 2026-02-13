#include "core/learning/online_ranker.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bs {

namespace {

constexpr int kDefaultFeatureDim = 13;

QVector<TrainingExample> splitTrain(const QVector<TrainingExample>& samples)
{
    QVector<TrainingExample> out;
    out.reserve(samples.size());
    for (int i = 0; i < samples.size(); ++i) {
        if (i % 5 != 0) {
            out.push_back(samples.at(i));
        }
    }
    return out;
}

QVector<TrainingExample> splitHoldout(const QVector<TrainingExample>& samples)
{
    QVector<TrainingExample> out;
    out.reserve(samples.size() / 5 + 1);
    for (int i = 0; i < samples.size(); ++i) {
        if (i % 5 == 0) {
            out.push_back(samples.at(i));
        }
    }
    return out;
}

} // namespace

OnlineRanker::OnlineRanker(QString modelPath)
    : m_modelPath(std::move(modelPath))
{
    QDir activeDir = QFileInfo(m_modelPath).absoluteDir();
    QDir modelRoot = activeDir;
    if (modelRoot.cdUp()) {
        m_candidatePath = modelRoot.filePath(QStringLiteral("candidate/weights.json"));
    } else {
        m_candidatePath = activeDir.filePath(QStringLiteral("candidate/weights.json"));
    }
    m_active.w.fill(0.0, kDefaultFeatureDim);
    m_active.bias = 0.0;
    m_active.version = QStringLiteral("cold_start");
    m_active.valid = false;
}

bool OnlineRanker::load()
{
    QFile file(m_modelPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    const QJsonObject obj = doc.object();
    const QJsonArray weights = obj.value(QStringLiteral("weights")).toArray();
    if (weights.isEmpty()) {
        return false;
    }

    QVector<double> parsed;
    parsed.reserve(weights.size());
    for (const QJsonValue& value : weights) {
        parsed.push_back(value.toDouble(0.0));
    }

    m_active.w = std::move(parsed);
    m_active.bias = obj.value(QStringLiteral("bias")).toDouble(0.0);
    m_active.version = obj.value(QStringLiteral("version")).toString(QStringLiteral("cold_start"));
    m_active.valid = true;
    return true;
}

bool OnlineRanker::save() const
{
    return saveWeights(m_active, m_modelPath);
}

bool OnlineRanker::saveWeights(const Weights& model, const QString& path) const
{
    if (!model.valid || model.w.isEmpty()) {
        return false;
    }

    QJsonArray weights;
    for (double value : model.w) {
        weights.append(value);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = model.version;
    root[QStringLiteral("updatedAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("bias")] = model.bias;
    root[QStringLiteral("weights")] = weights;

    QFile file(path);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

bool OnlineRanker::hasModel() const
{
    return m_active.valid && !m_active.w.isEmpty();
}

QString OnlineRanker::modelVersion() const
{
    return m_active.version;
}

int OnlineRanker::featureDim() const
{
    return m_active.w.isEmpty() ? 0 : m_active.w.size();
}

double OnlineRanker::scoreRaw(const Weights& model, const QVector<double>& features)
{
    if (!model.valid || model.w.isEmpty() || features.isEmpty()) {
        return 0.0;
    }

    const int dim = std::min(model.w.size(), features.size());
    double acc = model.bias;
    for (int i = 0; i < dim; ++i) {
        acc += model.w.at(i) * features.at(i);
    }
    return acc;
}

double OnlineRanker::sigmoid(double x)
{
    if (x >= 0.0) {
        const double z = std::exp(-x);
        return 1.0 / (1.0 + z);
    }
    const double z = std::exp(x);
    return z / (1.0 + z);
}

double OnlineRanker::clamp(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

double OnlineRanker::score(const QVector<double>& features) const
{
    if (!hasModel()) {
        return 0.5;
    }
    return sigmoid(scoreRaw(m_active, features));
}

double OnlineRanker::boost(const QVector<double>& features, double blendAlpha) const
{
    if (!hasModel() || blendAlpha <= 0.0) {
        return 0.0;
    }

    const double p = score(features);
    const double centered = p - 0.5;
    return 24.0 * clamp(blendAlpha, 0.0, 1.0) * centered;
}

double OnlineRanker::logLoss(const Weights& model,
                             const QVector<TrainingExample>& examples,
                             int* usedExamples,
                             double* avgPredictionLatencyUs,
                             double* predictionFailureRate,
                             double* probabilitySaturationRate)
{
    if (usedExamples) {
        *usedExamples = 0;
    }
    if (avgPredictionLatencyUs) {
        *avgPredictionLatencyUs = 0.0;
    }
    if (predictionFailureRate) {
        *predictionFailureRate = 0.0;
    }
    if (probabilitySaturationRate) {
        *probabilitySaturationRate = 0.0;
    }
    if (!model.valid || model.w.isEmpty() || examples.isEmpty()) {
        return 0.0;
    }

    double loss = 0.0;
    int used = 0;
    int attempted = 0;
    int failed = 0;
    int saturated = 0;
    double totalLatencyUs = 0.0;
    for (const TrainingExample& ex : examples) {
        if (ex.label < 0 || ex.denseFeatures.isEmpty()) {
            continue;
        }
        ++attempted;
        const double y = ex.label > 0 ? 1.0 : 0.0;
        const auto startedAt = std::chrono::steady_clock::now();
        const double rawScore = scoreRaw(model, ex.denseFeatures);
        const double rawProbability = sigmoid(rawScore);
        const auto endedAt = std::chrono::steady_clock::now();
        totalLatencyUs += static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(endedAt - startedAt).count());

        if (!std::isfinite(rawScore) || !std::isfinite(rawProbability)) {
            ++failed;
            continue;
        }
        const double p = clamp(rawProbability, 1e-6, 1.0 - 1e-6);
        if (p <= 1e-4 || p >= (1.0 - 1e-4)) {
            ++saturated;
        }
        const double weight = std::max(0.05, ex.weight);
        loss += -weight * (y * std::log(p) + (1.0 - y) * std::log(1.0 - p));
        ++used;
    }

    if (usedExamples) {
        *usedExamples = used;
    }
    if (avgPredictionLatencyUs) {
        *avgPredictionLatencyUs = used > 0
            ? (totalLatencyUs / static_cast<double>(used))
            : 0.0;
    }
    if (predictionFailureRate) {
        *predictionFailureRate = attempted > 0
            ? (static_cast<double>(failed) / static_cast<double>(attempted))
            : 0.0;
    }
    if (probabilitySaturationRate) {
        *probabilitySaturationRate = used > 0
            ? (static_cast<double>(saturated) / static_cast<double>(used))
            : 0.0;
    }
    return used > 0 ? loss / static_cast<double>(used) : 0.0;
}

OnlineRanker::Weights OnlineRanker::trainCandidate(const Weights& seed,
                                                    const QVector<TrainingExample>& trainSet,
                                                    const TrainConfig& config)
{
    Weights candidate = seed;
    if (candidate.w.isEmpty()) {
        candidate.w.fill(0.0, kDefaultFeatureDim);
    }
    candidate.valid = true;

    const int dim = candidate.w.size();
    const double lr = clamp(config.learningRate, 1e-4, 1.0);
    const double l2 = clamp(config.l2, 0.0, 1.0);
    const int epochs = std::max(1, config.epochs);

    for (int epoch = 0; epoch < epochs; ++epoch) {
        for (const TrainingExample& ex : trainSet) {
            if (ex.label < 0 || ex.denseFeatures.isEmpty()) {
                continue;
            }
            const double y = ex.label > 0 ? 1.0 : 0.0;
            const double p = sigmoid(scoreRaw(candidate, ex.denseFeatures));
            const double err = p - y;
            const double weight = std::max(0.05, ex.weight);

            for (int i = 0; i < dim; ++i) {
                const double feature = (i < ex.denseFeatures.size()) ? ex.denseFeatures.at(i) : 0.0;
                const double grad = (err * feature * weight) + (l2 * candidate.w[i]);
                candidate.w[i] -= (lr * grad);
            }
            candidate.bias -= (lr * err * weight);
        }
    }

    candidate.version = QStringLiteral("online_ranker_%1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddhhmmss")));
    return candidate;
}

bool OnlineRanker::trainAndPromote(const QVector<TrainingExample>& samples,
                                   const TrainConfig& config,
                                   TrainMetrics* activeMetrics,
                                   TrainMetrics* candidateMetrics,
                                   QString* rejectReason)
{
    if (rejectReason) {
        rejectReason->clear();
    }

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

    if (samples.size() < std::max(20, config.minExamples)) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("insufficient_examples");
        }
        return false;
    }

    int positiveCount = 0;
    for (const TrainingExample& ex : samples) {
        if (ex.label > 0) {
            ++positiveCount;
        }
    }
    if (positiveCount < 12) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("insufficient_positive_examples");
        }
        return false;
    }

    const QVector<TrainingExample> trainSet = splitTrain(samples);
    const QVector<TrainingExample> holdoutSet = splitHoldout(samples);
    if (trainSet.isEmpty() || holdoutSet.isEmpty()) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("invalid_train_holdout_split");
        }
        return false;
    }

    Weights seed = m_active;
    if (!seed.valid || seed.w.isEmpty()) {
        seed.w.fill(0.0, kDefaultFeatureDim);
        seed.bias = 0.0;
        seed.valid = true;
        seed.version = QStringLiteral("bootstrap");
    }

    Weights candidate = trainCandidate(seed, trainSet, config);
    saveWeights(candidate, m_candidatePath);

    int usedActive = 0;
    int usedCandidate = 0;
    double activeLatencyUs = 0.0;
    double candidateLatencyUs = 0.0;
    double activeFailureRate = 0.0;
    double candidateFailureRate = 0.0;
    double activeSaturationRate = 0.0;
    double candidateSaturationRate = 0.0;
    const double activeLoss = m_active.valid
        ? logLoss(m_active,
                  holdoutSet,
                  &usedActive,
                  &activeLatencyUs,
                  &activeFailureRate,
                  &activeSaturationRate)
        : 1.0;
    const double candidateLoss = logLoss(candidate,
                                         holdoutSet,
                                         &usedCandidate,
                                         &candidateLatencyUs,
                                         &candidateFailureRate,
                                         &candidateSaturationRate);

    if (activeMetrics) {
        activeMetrics->examples = usedActive;
        activeMetrics->logLoss = activeLoss;
        activeMetrics->avgPredictionLatencyUs = activeLatencyUs;
        activeMetrics->predictionFailureRate = activeFailureRate;
        activeMetrics->probabilitySaturationRate = activeSaturationRate;
    }
    if (candidateMetrics) {
        candidateMetrics->examples = usedCandidate;
        candidateMetrics->logLoss = candidateLoss;
        candidateMetrics->avgPredictionLatencyUs = candidateLatencyUs;
        candidateMetrics->predictionFailureRate = candidateFailureRate;
        candidateMetrics->probabilitySaturationRate = candidateSaturationRate;
    }

    if (!std::isfinite(candidateLoss) || usedCandidate <= 0) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("candidate_stability_invalid_eval");
        }
        return false;
    }

    const double latencyBudgetUs = clamp(config.promotionLatencyUsMax, 10.0, 1000000.0);
    const double latencyRegressionPct = clamp(config.promotionLatencyRegressionPctMax, 0.0, 1000.0);
    const double failureRateMax = clamp(config.promotionPredictionFailureRateMax, 0.0, 1.0);
    const double saturationRateMax = clamp(config.promotionSaturationRateMax, 0.0, 1.0);

    if (candidateLatencyUs > latencyBudgetUs) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("candidate_latency_budget_exceeded");
        }
        return false;
    }
    if (usedActive > 0 && activeLatencyUs > 0.0) {
        const double maxAllowedLatencyUs =
            activeLatencyUs * (1.0 + (latencyRegressionPct / 100.0));
        if (candidateLatencyUs > maxAllowedLatencyUs) {
            if (rejectReason) {
                *rejectReason = QStringLiteral("candidate_latency_regression_exceeded");
            }
            return false;
        }
    }
    if (candidateFailureRate > failureRateMax) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("candidate_stability_failure_rate_exceeded");
        }
        return false;
    }
    if (candidateSaturationRate > saturationRateMax) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("candidate_stability_saturation_rate_exceeded");
        }
        return false;
    }

    const bool promote = !m_active.valid || (candidateLoss + 0.002 < activeLoss);
    if (!promote) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("candidate_not_better_than_active");
        }
        return false;
    }

    m_active = std::move(candidate);
    m_active.valid = true;
    if (!save()) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("persist_active_model_failed");
        }
        return false;
    }
    return true;
}

} // namespace bs
