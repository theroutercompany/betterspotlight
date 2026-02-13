#include "core/learning/coreml_ranker.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bs {

namespace {

constexpr int kDefaultFeatureDim = 13;
constexpr double kPromotionMargin = 0.002;
constexpr int kUpdateTimeoutSeconds = 180;

QString toQString(NSString* value)
{
    return value ? QString::fromUtf8(value.UTF8String) : QString();
}

NSString* toNSString(const QString& value)
{
    return [NSString stringWithUTF8String:value.toUtf8().constData()];
}

QString nowVersionString()
{
    return QStringLiteral("coreml_online_ranker_%1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddhhmmss")));
}

double clamp(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

QVector<TrainingExample> splitTrain(const QVector<TrainingExample>& samples)
{
    QVector<TrainingExample> out;
    out.reserve(samples.size());
    for (int i = 0; i < samples.size(); ++i) {
        if ((i % 5) != 0) {
            out.push_back(samples.at(i));
        }
    }
    return out;
}

QVector<TrainingExample> splitHoldout(const QVector<TrainingExample>& samples)
{
    QVector<TrainingExample> out;
    out.reserve((samples.size() / 5) + 1);
    for (int i = 0; i < samples.size(); ++i) {
        if ((i % 5) == 0) {
            out.push_back(samples.at(i));
        }
    }
    return out;
}

bool copyRecursive(const QString& fromPath, const QString& toPath, QString* errorOut)
{
    NSFileManager* fileManager = [NSFileManager defaultManager];
    NSError* error = nil;
    NSString* src = toNSString(fromPath);
    NSString* dst = toNSString(toPath);

    if ([fileManager fileExistsAtPath:dst]) {
        if (![fileManager removeItemAtPath:dst error:&error]) {
            if (errorOut) {
                *errorOut = QStringLiteral("remove_existing_model_failed:%1")
                    .arg(toQString(error.localizedDescription));
            }
            return false;
        }
    }

    const QString parentDir = QFileInfo(toPath).absolutePath();
    if (!QDir().mkpath(parentDir)) {
        if (errorOut) {
            *errorOut = QStringLiteral("create_parent_dir_failed:%1").arg(parentDir);
        }
        return false;
    }

    if (![fileManager copyItemAtPath:src toPath:dst error:&error]) {
        if (errorOut) {
            *errorOut = QStringLiteral("copy_model_failed:%1")
                .arg(toQString(error.localizedDescription));
        }
        return false;
    }
    return true;
}

QJsonObject readMetadata(const QString& metadataPath)
{
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

bool writeMetadata(const QString& metadataPath, const QJsonObject& metadata, QString* errorOut)
{
    QFile file(metadataPath);
    if (!QDir().mkpath(QFileInfo(metadataPath).absolutePath())) {
        if (errorOut) {
            *errorOut = QStringLiteral("create_metadata_dir_failed");
        }
        return false;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("open_metadata_failed");
        }
        return false;
    }
    file.write(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    return true;
}

MLModel* loadModelAtPath(const QString& modelPath, QString* errorOut)
{
    NSError* error = nil;
    MLModelConfiguration* config = [[MLModelConfiguration alloc] init];
    if (@available(macOS 13.0, *)) {
        config.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
    } else {
        config.computeUnits = MLComputeUnitsAll;
    }
    MLModel* model = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:toNSString(modelPath)]
                                        configuration:config
                                                error:&error];
    [config release];
    if (!model) {
        if (errorOut) {
            *errorOut = QStringLiteral("load_model_failed:%1")
                .arg(toQString(error.localizedDescription));
        }
        return nil;
    }
    return [model retain];
}

bool writeCompiledModel(MLModel<MLWritable>* model, const QString& outputPath, QString* errorOut)
{
    NSError* error = nil;
    NSFileManager* fileManager = [NSFileManager defaultManager];
    NSString* output = toNSString(outputPath);
    if ([fileManager fileExistsAtPath:output]) {
        [fileManager removeItemAtPath:output error:nil];
    }
    if (![model writeToURL:[NSURL fileURLWithPath:output] error:&error]) {
        if (errorOut) {
            *errorOut = QStringLiteral("write_updated_model_failed:%1")
                .arg(toQString(error.localizedDescription));
        }
        return false;
    }
    return true;
}

double scoreWithModel(MLModel* model,
                      const QString& inputFeatureName,
                      int featureDim,
                      const QVector<double>& features,
                      bool* okOut)
{
    if (okOut) {
        *okOut = false;
    }
    if (!model || featureDim <= 0) {
        return 0.5;
    }

    NSError* error = nil;
    MLMultiArray* inputArray = [[MLMultiArray alloc] initWithShape:@[@(featureDim)]
                                                           dataType:MLMultiArrayDataTypeDouble
                                                              error:&error];
    if (!inputArray || error) {
        [inputArray release];
        return 0.5;
    }

    for (int i = 0; i < featureDim; ++i) {
        const double value = (i < features.size()) ? features.at(i) : 0.0;
        inputArray[static_cast<NSInteger>(i)] = @(value);
    }

    NSDictionary* inputDict = @{
        toNSString(inputFeatureName): [MLFeatureValue featureValueWithMultiArray:inputArray],
    };
    MLDictionaryFeatureProvider* provider =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:inputDict error:&error];
    [inputArray release];
    if (!provider || error) {
        [provider release];
        return 0.5;
    }

    id<MLFeatureProvider> output = [model predictionFromFeatures:provider error:&error];
    [provider release];
    if (!output || error) {
        return 0.5;
    }

    double probability = 0.5;
    NSString* probabilityName = model.modelDescription.predictedProbabilitiesName;
    MLFeatureValue* value = nil;
    if (probabilityName) {
        value = [output featureValueForName:probabilityName];
    }
    if (!value) {
        NSDictionary* outputs = model.modelDescription.outputDescriptionsByName;
        for (NSString* key in outputs) {
            MLFeatureValue* candidate = [output featureValueForName:key];
            if (!candidate) {
                continue;
            }
            if (candidate.type == MLFeatureTypeDictionary
                || candidate.type == MLFeatureTypeDouble
                || candidate.type == MLFeatureTypeMultiArray) {
                value = candidate;
                break;
            }
        }
    }

    if (value) {
        if (value.type == MLFeatureTypeDictionary) {
            NSDictionary* dict = value.dictionaryValue;
            NSNumber* positive = dict[@1];
            if (!positive) {
                positive = dict[@"1"];
            }
            if (!positive) {
                positive = dict[@"true"];
            }
            if (!positive) {
                positive = dict[@"positive"];
            }
            if (!positive && dict.count > 0) {
                positive = dict.allValues.firstObject;
            }
            if (positive) {
                probability = positive.doubleValue;
            }
        } else if (value.type == MLFeatureTypeDouble) {
            probability = value.doubleValue;
        } else if (value.type == MLFeatureTypeMultiArray) {
            MLMultiArray* multiArray = value.multiArrayValue;
            if (multiArray && multiArray.count > 0) {
                const NSInteger idx = (multiArray.count > 1) ? 1 : 0;
                probability = [multiArray[idx] doubleValue];
            }
        }
    }

    if (okOut) {
        *okOut = true;
    }
    return clamp(probability, 0.0, 1.0);
}

struct EvalSummary {
    int usedExamples = 0;
    int attemptedExamples = 0;
    int failedExamples = 0;
    int saturatedExamples = 0;
    double logLoss = 0.0;
    double avgPredictionLatencyUs = 0.0;
    double predictionFailureRate = 0.0;
    double probabilitySaturationRate = 0.0;
};

EvalSummary evaluateModel(MLModel* model,
                          const QString& inputFeatureName,
                          int featureDim,
                          const QVector<TrainingExample>& examples)
{
    EvalSummary summary;
    if (!model || examples.isEmpty()) {
        return summary;
    }

    double loss = 0.0;
    double totalLatencyUs = 0.0;
    int used = 0;
    for (const TrainingExample& ex : examples) {
        if (ex.label < 0 || ex.denseFeatures.isEmpty()) {
            continue;
        }
        ++summary.attemptedExamples;
        bool ok = false;
        const auto startedAt = std::chrono::steady_clock::now();
        const double p = clamp(scoreWithModel(model,
                                              inputFeatureName,
                                              featureDim,
                                              ex.denseFeatures,
                                              &ok),
                               1e-6,
                               1.0 - 1e-6);
        const auto endedAt = std::chrono::steady_clock::now();
        totalLatencyUs += static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(endedAt - startedAt).count());
        if (!ok) {
            ++summary.failedExamples;
            continue;
        }
        if (!std::isfinite(p)) {
            ++summary.failedExamples;
            continue;
        }
        if (p <= 1e-4 || p >= (1.0 - 1e-4)) {
            ++summary.saturatedExamples;
        }
        const double y = ex.label > 0 ? 1.0 : 0.0;
        const double weight = std::max(0.05, ex.weight);
        loss += -weight * ((y * std::log(p)) + ((1.0 - y) * std::log(1.0 - p)));
        ++used;
    }

    summary.usedExamples = used;
    summary.logLoss = used > 0 ? (loss / static_cast<double>(used)) : 0.0;
    summary.avgPredictionLatencyUs = used > 0
        ? (totalLatencyUs / static_cast<double>(used))
        : 0.0;
    summary.predictionFailureRate = summary.attemptedExamples > 0
        ? (static_cast<double>(summary.failedExamples)
           / static_cast<double>(summary.attemptedExamples))
        : 0.0;
    summary.probabilitySaturationRate = used > 0
        ? (static_cast<double>(summary.saturatedExamples) / static_cast<double>(used))
        : 0.0;
    return summary;
}

id<MLBatchProvider> makeTrainingBatch(const QVector<TrainingExample>& trainSet,
                                      const QString& inputFeatureName,
                                      const QString& labelFeatureName,
                                      int featureDim,
                                      QString* errorOut)
{
    NSMutableArray* providers = [[NSMutableArray alloc] init];
    for (const TrainingExample& ex : trainSet) {
        if (ex.label < 0 || ex.denseFeatures.isEmpty()) {
            continue;
        }

        NSError* error = nil;
        MLMultiArray* inputArray = [[MLMultiArray alloc] initWithShape:@[@(featureDim)]
                                                               dataType:MLMultiArrayDataTypeDouble
                                                                  error:&error];
        if (!inputArray || error) {
            [inputArray release];
            if (errorOut) {
                *errorOut = QStringLiteral("build_training_batch_failed:%1")
                    .arg(toQString(error.localizedDescription));
            }
            [providers release];
            return nil;
        }

        for (int i = 0; i < featureDim; ++i) {
            const double value = (i < ex.denseFeatures.size()) ? ex.denseFeatures.at(i) : 0.0;
            inputArray[static_cast<NSInteger>(i)] = @(value);
        }

        NSDictionary* dict = @{
            toNSString(inputFeatureName): [MLFeatureValue featureValueWithMultiArray:inputArray],
            toNSString(labelFeatureName): [MLFeatureValue featureValueWithInt64:(ex.label > 0 ? 1 : 0)],
        };
        MLDictionaryFeatureProvider* provider =
            [[MLDictionaryFeatureProvider alloc] initWithDictionary:dict error:&error];
        [inputArray release];
        if (!provider || error) {
            [provider release];
            if (errorOut) {
                *errorOut = QStringLiteral("build_training_provider_failed:%1")
                    .arg(toQString(error.localizedDescription));
            }
            [providers release];
            return nil;
        }
        [providers addObject:provider];
        [provider release];
    }

    MLArrayBatchProvider* batch =
        [[MLArrayBatchProvider alloc] initWithFeatureProviderArray:providers];
    [providers release];
    return batch;
}

} // namespace

struct CoreMlRanker::Impl {
    QString modelRootDir;
    QString activeModelPath;
    QString candidateModelPath;
    QString bootstrapModelPath;
    QString activeMetadataPath;
    QString candidateMetadataPath;
    QString bootstrapMetadataPath;
    QString inputFeatureName = QStringLiteral("features");
    QString labelFeatureName = QStringLiteral("label");
    QString modelVersion;
    int featureDim = kDefaultFeatureDim;
    bool updatable = false;
    MLModel* activeModel = nil;

    ~Impl()
    {
        if (activeModel) {
            [activeModel release];
            activeModel = nil;
        }
    }
};

CoreMlRanker::CoreMlRanker(QString modelRootDir)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->modelRootDir = std::move(modelRootDir);
    m_impl->activeModelPath = QDir(m_impl->modelRootDir)
        .filePath(QStringLiteral("active/online_ranker_v1.mlmodelc"));
    m_impl->candidateModelPath = QDir(m_impl->modelRootDir)
        .filePath(QStringLiteral("candidate/online_ranker_v1.mlmodelc"));
    m_impl->bootstrapModelPath = QDir(m_impl->modelRootDir)
        .filePath(QStringLiteral("bootstrap/online_ranker_v1.mlmodelc"));
    m_impl->activeMetadataPath = QDir(m_impl->modelRootDir)
        .filePath(QStringLiteral("active/metadata.json"));
    m_impl->candidateMetadataPath = QDir(m_impl->modelRootDir)
        .filePath(QStringLiteral("candidate/metadata.json"));
    m_impl->bootstrapMetadataPath = QDir(m_impl->modelRootDir)
        .filePath(QStringLiteral("bootstrap/metadata.json"));
}

CoreMlRanker::~CoreMlRanker() = default;

bool CoreMlRanker::initialize(QString* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }

    if (!QFileInfo::exists(m_impl->activeModelPath)) {
        if (!QFileInfo::exists(m_impl->bootstrapModelPath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("coreml_bootstrap_model_missing");
            }
            return false;
        }
        QString copyError;
        if (!copyRecursive(m_impl->bootstrapModelPath, m_impl->activeModelPath, &copyError)) {
            if (errorOut) {
                *errorOut = copyError;
            }
            return false;
        }
        const QJsonObject bootstrapMetadata = readMetadata(m_impl->bootstrapMetadataPath);
        if (!bootstrapMetadata.isEmpty()) {
            QString metadataError;
            writeMetadata(m_impl->activeMetadataPath, bootstrapMetadata, &metadataError);
        }
    }

    QString loadError;
    MLModel* loaded = loadModelAtPath(m_impl->activeModelPath, &loadError);
    if (!loaded) {
        if (errorOut) {
            *errorOut = loadError;
        }
        return false;
    }

    if (m_impl->activeModel) {
        [m_impl->activeModel release];
        m_impl->activeModel = nil;
    }
    m_impl->activeModel = loaded;
    m_impl->updatable = m_impl->activeModel.modelDescription.isUpdatable;

    const QJsonObject metadata = readMetadata(m_impl->activeMetadataPath);
    if (!metadata.isEmpty()) {
        m_impl->modelVersion = metadata.value(QStringLiteral("version")).toString(nowVersionString());
        m_impl->inputFeatureName = metadata.value(QStringLiteral("inputFeatureName"))
                                       .toString(QStringLiteral("features"));
        m_impl->labelFeatureName = metadata.value(QStringLiteral("labelFeatureName"))
                                       .toString(QStringLiteral("label"));
        const int dim = metadata.value(QStringLiteral("featureDim")).toInt(kDefaultFeatureDim);
        m_impl->featureDim = std::max(1, dim);
    } else {
        m_impl->modelVersion = nowVersionString();
    }

    NSDictionary* inputDescriptions = m_impl->activeModel.modelDescription.inputDescriptionsByName;
    if (!inputDescriptions || inputDescriptions.count == 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("coreml_model_has_no_inputs");
        }
        return false;
    }

    NSString* resolvedInput = toNSString(m_impl->inputFeatureName);
    if (!inputDescriptions[resolvedInput]) {
        resolvedInput = inputDescriptions.allKeys.firstObject;
        m_impl->inputFeatureName = toQString(resolvedInput);
    }

    MLFeatureDescription* inputDesc = inputDescriptions[resolvedInput];
    if (inputDesc && inputDesc.type == MLFeatureTypeMultiArray
        && inputDesc.multiArrayConstraint.shape.count > 0) {
        const NSInteger inferredDim = [inputDesc.multiArrayConstraint.shape.firstObject integerValue];
        if (inferredDim > 0) {
            m_impl->featureDim = static_cast<int>(inferredDim);
        }
    }

    NSDictionary* trainingInputs = m_impl->activeModel.modelDescription.trainingInputDescriptionsByName;
    if (trainingInputs && trainingInputs.count > 0) {
        NSString* preferredLabel = toNSString(m_impl->labelFeatureName);
        if (!trainingInputs[preferredLabel]) {
            for (NSString* key in trainingInputs) {
                if (key && ![key isEqualToString:resolvedInput]) {
                    preferredLabel = key;
                    break;
                }
            }
            if (preferredLabel) {
                m_impl->labelFeatureName = toQString(preferredLabel);
            }
        }
    }

    return true;
}

bool CoreMlRanker::hasModel() const
{
    return m_impl->activeModel != nil;
}

bool CoreMlRanker::isUpdatable() const
{
    return m_impl->updatable;
}

QString CoreMlRanker::modelVersion() const
{
    return m_impl->modelVersion;
}

int CoreMlRanker::featureDim() const
{
    return m_impl->featureDim;
}

double CoreMlRanker::score(const QVector<double>& features, bool* okOut) const
{
    return scoreWithModel(m_impl->activeModel,
                          m_impl->inputFeatureName,
                          m_impl->featureDim,
                          features,
                          okOut);
}

double CoreMlRanker::boost(const QVector<double>& features, double blendAlpha, bool* okOut) const
{
    const double probability = score(features, okOut);
    if (okOut && !*okOut) {
        return 0.0;
    }
    const double centered = probability - 0.5;
    return 24.0 * clamp(blendAlpha, 0.0, 1.0) * centered;
}

bool CoreMlRanker::trainAndPromote(const QVector<TrainingExample>& samples,
                                   const OnlineRanker::TrainConfig& config,
                                   OnlineRanker::TrainMetrics* activeMetrics,
                                   OnlineRanker::TrainMetrics* candidateMetrics,
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

    if (!hasModel()) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("coreml_model_unavailable");
        }
        return false;
    }
    if (!m_impl->updatable) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("coreml_model_not_updatable");
        }
        return false;
    }

    const int minExamples = std::max(20, config.minExamples);
    if (samples.size() < minExamples) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("insufficient_examples");
        }
        return false;
    }

    int positiveCount = 0;
    for (const TrainingExample& sample : samples) {
        if (sample.label > 0) {
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

    QString batchError;
    id<MLBatchProvider> batch = makeTrainingBatch(trainSet,
                                                  m_impl->inputFeatureName,
                                                  m_impl->labelFeatureName,
                                                  m_impl->featureDim,
                                                  &batchError);
    if (!batch) {
        if (rejectReason) {
            *rejectReason = batchError.isEmpty()
                ? QStringLiteral("build_training_batch_failed")
                : batchError;
        }
        return false;
    }

    MLModelConfiguration* updateConfig = [[MLModelConfiguration alloc] init];
    if (@available(macOS 13.0, *)) {
        updateConfig.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
    } else {
        updateConfig.computeUnits = MLComputeUnitsAll;
    }
    NSMutableDictionary* params = [[NSMutableDictionary alloc] init];
    params[MLParameterKey.epochs] = @(std::max(1, config.epochs));
    params[MLParameterKey.learningRate] = @(clamp(config.learningRate, 1e-4, 1.0));
    params[MLParameterKey.miniBatchSize] = @(32);
    params[MLParameterKey.shuffle] = @YES;
    updateConfig.parameters = params;
    [params release];

    __block MLUpdateContext* completionContext = nil;
    dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);
    NSError* taskError = nil;
    MLUpdateTask* updateTask =
        [MLUpdateTask updateTaskForModelAtURL:[NSURL fileURLWithPath:toNSString(m_impl->activeModelPath)]
                                 trainingData:batch
                                configuration:updateConfig
                            completionHandler:^(MLUpdateContext* context) {
            completionContext = [context retain];
            dispatch_semaphore_signal(completionSemaphore);
        }
                                        error:&taskError];
    [updateConfig release];
    [(id)batch release];

    if (!updateTask || taskError) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("coreml_update_task_create_failed:%1")
                .arg(toQString(taskError.localizedDescription));
        }
        return false;
    }

    [updateTask resume];
    const long waitResult = dispatch_semaphore_wait(
        completionSemaphore,
        dispatch_time(DISPATCH_TIME_NOW,
                      static_cast<int64_t>(kUpdateTimeoutSeconds) * NSEC_PER_SEC));
    if (waitResult != 0) {
        [updateTask cancel];
        if (rejectReason) {
            *rejectReason = QStringLiteral("coreml_update_task_timeout");
        }
        if (completionContext) {
            [completionContext release];
        }
        return false;
    }

    if (updateTask.error || !completionContext) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("coreml_update_task_failed:%1")
                .arg(toQString(updateTask.error.localizedDescription));
        }
        if (completionContext) {
            [completionContext release];
        }
        return false;
    }

    QString writeError;
    if (!writeCompiledModel(completionContext.model, m_impl->candidateModelPath, &writeError)) {
        if (rejectReason) {
            *rejectReason = writeError;
        }
        [completionContext release];
        return false;
    }
    [completionContext release];

    const QString candidateVersion = nowVersionString();
    QJsonObject candidateMetadata;
    candidateMetadata[QStringLiteral("version")] = candidateVersion;
    candidateMetadata[QStringLiteral("backend")] = QStringLiteral("coreml");
    candidateMetadata[QStringLiteral("updatedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    candidateMetadata[QStringLiteral("featureDim")] = m_impl->featureDim;
    candidateMetadata[QStringLiteral("inputFeatureName")] = m_impl->inputFeatureName;
    candidateMetadata[QStringLiteral("labelFeatureName")] = m_impl->labelFeatureName;
    QString metadataError;
    if (!writeMetadata(m_impl->candidateMetadataPath, candidateMetadata, &metadataError)) {
        if (rejectReason) {
            *rejectReason = metadataError;
        }
        return false;
    }

    QString candidateLoadError;
    MLModel* candidateModel = loadModelAtPath(m_impl->candidateModelPath, &candidateLoadError);
    if (!candidateModel) {
        if (rejectReason) {
            *rejectReason = candidateLoadError;
        }
        return false;
    }

    const EvalSummary activeEval = evaluateModel(m_impl->activeModel,
                                                 m_impl->inputFeatureName,
                                                 m_impl->featureDim,
                                                 holdoutSet);
    const EvalSummary candidateEval = evaluateModel(candidateModel,
                                                    m_impl->inputFeatureName,
                                                    m_impl->featureDim,
                                                    holdoutSet);
    if (activeMetrics) {
        activeMetrics->examples = activeEval.usedExamples;
        activeMetrics->logLoss = activeEval.logLoss;
        activeMetrics->avgPredictionLatencyUs = activeEval.avgPredictionLatencyUs;
        activeMetrics->predictionFailureRate = activeEval.predictionFailureRate;
        activeMetrics->probabilitySaturationRate = activeEval.probabilitySaturationRate;
    }
    if (candidateMetrics) {
        candidateMetrics->examples = candidateEval.usedExamples;
        candidateMetrics->logLoss = candidateEval.logLoss;
        candidateMetrics->avgPredictionLatencyUs = candidateEval.avgPredictionLatencyUs;
        candidateMetrics->predictionFailureRate = candidateEval.predictionFailureRate;
        candidateMetrics->probabilitySaturationRate = candidateEval.probabilitySaturationRate;
    }

    OnlineRanker::TrainMetrics activeEvalMetrics;
    activeEvalMetrics.examples = activeEval.usedExamples;
    activeEvalMetrics.logLoss = activeEval.logLoss;
    activeEvalMetrics.avgPredictionLatencyUs = activeEval.avgPredictionLatencyUs;
    activeEvalMetrics.predictionFailureRate = activeEval.predictionFailureRate;
    activeEvalMetrics.probabilitySaturationRate = activeEval.probabilitySaturationRate;

    OnlineRanker::TrainMetrics candidateEvalMetrics;
    candidateEvalMetrics.examples = candidateEval.usedExamples;
    candidateEvalMetrics.logLoss = candidateEval.logLoss;
    candidateEvalMetrics.avgPredictionLatencyUs = candidateEval.avgPredictionLatencyUs;
    candidateEvalMetrics.predictionFailureRate = candidateEval.predictionFailureRate;
    candidateEvalMetrics.probabilitySaturationRate = candidateEval.probabilitySaturationRate;

    if (!OnlineRanker::passesPromotionRuntimeGates(config,
                                                   activeEvalMetrics,
                                                   candidateEvalMetrics,
                                                   rejectReason)) {
        [candidateModel release];
        return false;
    }

    const bool promote = (candidateEval.logLoss + kPromotionMargin) < activeEval.logLoss;
    if (!promote) {
        [candidateModel release];
        if (rejectReason) {
            *rejectReason = QStringLiteral("candidate_not_better_than_active");
        }
        return false;
    }

    QString promoteError;
    if (!copyRecursive(m_impl->candidateModelPath, m_impl->activeModelPath, &promoteError)) {
        [candidateModel release];
        if (rejectReason) {
            *rejectReason = promoteError;
        }
        return false;
    }
    if (!copyRecursive(m_impl->candidateMetadataPath, m_impl->activeMetadataPath, &promoteError)) {
        [candidateModel release];
        if (rejectReason) {
            *rejectReason = promoteError;
        }
        return false;
    }

    if (m_impl->activeModel) {
        [m_impl->activeModel release];
        m_impl->activeModel = nil;
    }
    m_impl->activeModel = candidateModel;
    m_impl->modelVersion = candidateVersion;
    return true;
}

} // namespace bs
