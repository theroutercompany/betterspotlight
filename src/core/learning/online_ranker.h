#pragma once

#include "core/learning/behavior_types.h"

#include <QVector>
#include <QString>

namespace bs {

class OnlineRanker {
public:
    struct TrainConfig {
        int epochs = 3;
        double learningRate = 0.05;
        double l2 = 1e-4;
        int minExamples = 120;
        double promotionLatencyUsMax = 2500.0;
        double promotionLatencyRegressionPctMax = 35.0;
        double promotionPredictionFailureRateMax = 0.05;
        double promotionSaturationRateMax = 0.995;
    };

    struct TrainMetrics {
        int examples = 0;
        double logLoss = 0.0;
        double avgPredictionLatencyUs = 0.0;
        double predictionFailureRate = 0.0;
        double probabilitySaturationRate = 0.0;
    };

    explicit OnlineRanker(QString modelPath);

    bool load();
    bool save() const;

    bool hasModel() const;
    QString modelVersion() const;
    int featureDim() const;

    double score(const QVector<double>& features) const;
    double boost(const QVector<double>& features, double blendAlpha) const;

    bool trainAndPromote(const QVector<TrainingExample>& samples,
                         const TrainConfig& config,
                         TrainMetrics* activeMetrics,
                         TrainMetrics* candidateMetrics,
                         QString* rejectReason);

private:
    struct Weights {
        QVector<double> w;
        double bias = 0.0;
        QString version;
        bool valid = false;
    };

    static double sigmoid(double x);
    static double clamp(double value, double lo, double hi);
    static double logLoss(const Weights& model,
                          const QVector<TrainingExample>& examples,
                          int* usedExamples = nullptr,
                          double* avgPredictionLatencyUs = nullptr,
                          double* predictionFailureRate = nullptr,
                          double* probabilitySaturationRate = nullptr);
    static double scoreRaw(const Weights& model, const QVector<double>& features);
    static Weights trainCandidate(const Weights& seed,
                                  const QVector<TrainingExample>& trainSet,
                                  const TrainConfig& config);
    bool saveWeights(const Weights& model, const QString& path) const;

    QString m_modelPath;
    QString m_candidatePath;
    Weights m_active;
};

} // namespace bs
