#include "core/learning/learning_engine.h"

#include "core/feedback/interaction_tracker.h"
#include "core/learning/coreml_ranker.h"
#include "core/learning/online_ranker.h"
#include "core/shared/logging.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QTimeZone>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

namespace bs {

namespace {

constexpr int kFeatureDim = 13;
constexpr int kDefaultReplayCapacity = 4000;
constexpr int kDefaultFreshTrainingLimit = 1200;
constexpr int kDefaultReplaySampleLimit = 1200;
constexpr int kDefaultMaxTrainingBatchSize = 1200;
constexpr int kDefaultNegativeStaleSeconds = 30;
constexpr double kDefaultNegativeSampleRatio = 3.0;
constexpr int kDefaultHealthWindowDays = 7;
constexpr int kDefaultRecentCycleHistoryLimit = 50;
constexpr int kDefaultPromotionGateMinPositives = 80;
constexpr double kDefaultPromotionMinAttributedRate = 0.5;
constexpr double kDefaultPromotionMinContextDigestRate = 0.1;
constexpr double kDefaultPromotionLatencyUsMax = 2500.0;
constexpr double kDefaultPromotionLatencyRegressionPctMax = 35.0;
constexpr double kDefaultPromotionPredictionFailureRateMax = 0.05;
constexpr double kDefaultPromotionSaturationRateMax = 0.995;
constexpr int kIdleGapMs = 10000;
constexpr int kMinCycleIntervalMs = 60000;
constexpr int kPruneIntervalMs = 60 * 60 * 1000;
constexpr const char* kRolloutInstrumentationOnly = "instrumentation_only";
constexpr const char* kRolloutShadowTraining = "shadow_training";
constexpr const char* kRolloutBlendedRanking = "blended_ranking";
constexpr double kAttributionContextThreshold = 0.95;
constexpr double kAttributionDigestThreshold = 0.8;

struct BatchAttributionStats {
    int positiveExamples = 0;
    int contextHits = 0;
    int digestHits = 0;
    int queryOnlyHits = 0;
    int unattributedPositives = 0;
    double attributedRate = 0.0;
    double contextRate = 0.0;
    double digestRate = 0.0;
    double queryOnlyRate = 0.0;
    double unattributedRate = 0.0;
    double contextDigestRate = 0.0;
};

BatchAttributionStats collectBatchAttributionStats(const QVector<TrainingExample>& examples)
{
    BatchAttributionStats stats;
    for (const TrainingExample& example : examples) {
        if (example.label != 1) {
            continue;
        }
        ++stats.positiveExamples;
        const double confidence = std::clamp(example.attributionConfidence, 0.0, 1.0);
        if (confidence >= kAttributionContextThreshold) {
            ++stats.contextHits;
        } else if (confidence >= kAttributionDigestThreshold) {
            ++stats.digestHits;
        } else if (confidence > 0.0) {
            ++stats.queryOnlyHits;
        } else {
            ++stats.unattributedPositives;
        }
    }

    if (stats.positiveExamples > 0) {
        const double denom = static_cast<double>(stats.positiveExamples);
        const int attributed = stats.contextHits + stats.digestHits + stats.queryOnlyHits;
        stats.attributedRate = static_cast<double>(attributed) / denom;
        stats.contextRate = static_cast<double>(stats.contextHits) / denom;
        stats.digestRate = static_cast<double>(stats.digestHits) / denom;
        stats.queryOnlyRate = static_cast<double>(stats.queryOnlyHits) / denom;
        stats.unattributedRate = static_cast<double>(stats.unattributedPositives) / denom;
        stats.contextDigestRate = static_cast<double>(stats.contextHits + stats.digestHits) / denom;
    }

    return stats;
}

QJsonArray parseJsonArrayOrEmpty(const QString& encoded)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(encoded.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        return QJsonArray();
    }
    return doc.array();
}

double exposureBiasWeightForRank(int rank)
{
    const int safeRank = std::max(0, rank);
    // Approximate inverse-propensity correction: lower-ranked items get more weight.
    const double inversePropensity = std::log2(static_cast<double>(safeRank) + 2.0);
    return std::clamp(inversePropensity, 1.0, 4.0);
}

QVector<TrainingExample> sampleTrainingBatch(const QVector<TrainingExample>& examples,
                                             double negativeSampleRatio,
                                             int maxBatchSize)
{
    QVector<TrainingExample> positives;
    QVector<TrainingExample> negatives;
    positives.reserve(examples.size());
    negatives.reserve(examples.size());

    for (const TrainingExample& example : examples) {
        if (example.label > 0) {
            positives.push_back(example);
        } else if (example.label == 0) {
            negatives.push_back(example);
        }
    }

    const int batchCap = std::max(1, maxBatchSize);
    int targetNegatives = negatives.size();
    if (!positives.isEmpty()) {
        const double safeRatio = std::max(0.0, negativeSampleRatio);
        const int ratioBound = static_cast<int>(std::ceil(
            static_cast<double>(positives.size()) * safeRatio));
        targetNegatives = std::min(targetNegatives, std::max(0, ratioBound));
    }

    if (positives.size() >= batchCap) {
        positives.resize(batchCap);
        targetNegatives = 0;
    } else {
        const int remainingCapacity = std::max(0, batchCap - static_cast<int>(positives.size()));
        targetNegatives = std::min(targetNegatives, remainingCapacity);
    }

    if (targetNegatives < negatives.size()) {
        std::mt19937 rng(static_cast<std::mt19937::result_type>(
            QRandomGenerator::global()->generate()));
        std::shuffle(negatives.begin(), negatives.end(), rng);
        negatives.resize(targetNegatives);
    }

    QVector<TrainingExample> sampled;
    sampled.reserve(positives.size() + negatives.size());
    sampled += positives;
    sampled += negatives;

    if (sampled.size() > 1) {
        std::mt19937 rng(static_cast<std::mt19937::result_type>(
            QRandomGenerator::global()->generate()));
        std::shuffle(sampled.begin(), sampled.end(), rng);
    }
    return sampled;
}

double currentProcessCpuPct()
{
    QProcess process;
    process.start(QStringLiteral("/bin/ps"),
                  {QStringLiteral("-o"), QStringLiteral("%cpu="),
                   QStringLiteral("-p"),
                   QString::number(QCoreApplication::applicationPid())});
    if (!process.waitForFinished(750)) {
        process.kill();
        process.waitForFinished(200);
        return -1.0;
    }
    bool ok = false;
    const double value = QString::fromUtf8(process.readAllStandardOutput())
                             .trimmed()
                             .toDouble(&ok);
    return ok ? value : -1.0;
}

double currentProcessRssMb()
{
    QProcess process;
    process.start(QStringLiteral("/bin/ps"),
                  {QStringLiteral("-o"), QStringLiteral("rss="),
                   QStringLiteral("-p"),
                   QString::number(QCoreApplication::applicationPid())});
    if (!process.waitForFinished(750)) {
        process.kill();
        process.waitForFinished(200);
        return -1.0;
    }
    bool ok = false;
    const double rssKb = QString::fromUtf8(process.readAllStandardOutput())
                             .trimmed()
                             .toDouble(&ok);
    return ok ? (rssKb / 1024.0) : -1.0;
}

int currentThermalState()
{
#ifdef Q_OS_MACOS
    QProcess process;
    process.start(QStringLiteral("/usr/bin/pmset"), {QStringLiteral("-g"), QStringLiteral("therm")});
    if (!process.waitForFinished(1000)) {
        process.kill();
        process.waitForFinished(200);
        return -1;
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput()).toLower();
    if (output.contains(QStringLiteral("critical"))) {
        return 3;
    }
    if (output.contains(QStringLiteral("serious")) || output.contains(QStringLiteral("high"))) {
        return 2;
    }
    if (output.contains(QStringLiteral("fair")) || output.contains(QStringLiteral("medium"))) {
        return 1;
    }
    if (output.contains(QStringLiteral("nominal")) || output.contains(QStringLiteral("normal"))) {
        return 0;
    }
#endif
    return -1;
}

bool execSql(sqlite3* db, const char* sql)
{
    if (!db) {
        return false;
    }
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

QString canonicalRolloutMode(const QString& rawMode, bool* validOut = nullptr)
{
    if (validOut) {
        *validOut = true;
    }
    const QString normalized = rawMode.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QLatin1String(kRolloutInstrumentationOnly)) {
        return QString::fromLatin1(kRolloutInstrumentationOnly);
    }
    if (normalized == QLatin1String(kRolloutShadowTraining)) {
        return QString::fromLatin1(kRolloutShadowTraining);
    }
    if (normalized == QLatin1String(kRolloutBlendedRanking)) {
        return QString::fromLatin1(kRolloutBlendedRanking);
    }
    if (validOut) {
        *validOut = false;
    }
    return QString::fromLatin1(kRolloutInstrumentationOnly);
}

bool rolloutAllowsTraining(const QString& mode)
{
    return mode == QLatin1String(kRolloutShadowTraining)
        || mode == QLatin1String(kRolloutBlendedRanking);
}

bool rolloutAllowsServing(const QString& mode)
{
    return mode == QLatin1String(kRolloutBlendedRanking);
}

QJsonObject collectAttributionMetrics(sqlite3* db, int lookbackDays)
{
    QJsonObject metrics;
    metrics[QStringLiteral("windowDays")] = std::max(1, lookbackDays);
    metrics[QStringLiteral("positiveExamples")] = 0;
    metrics[QStringLiteral("attributedExamples")] = 0;
    metrics[QStringLiteral("contextHits")] = 0;
    metrics[QStringLiteral("digestHits")] = 0;
    metrics[QStringLiteral("queryOnlyHits")] = 0;
    metrics[QStringLiteral("unattributedPositives")] = 0;
    metrics[QStringLiteral("attributedRate")] = 0.0;
    metrics[QStringLiteral("contextHitRate")] = 0.0;
    metrics[QStringLiteral("digestHitRate")] = 0.0;
    metrics[QStringLiteral("queryOnlyRate")] = 0.0;
    metrics[QStringLiteral("unattributedRate")] = 0.0;

    if (!db) {
        return metrics;
    }

    static constexpr const char* kSql = R"(
        SELECT
            SUM(CASE WHEN label = 1 THEN 1 ELSE 0 END) AS positives,
            SUM(CASE WHEN label = 1 AND attribution_confidence >= ?2 THEN 1 ELSE 0 END)
                AS context_hits,
            SUM(CASE WHEN label = 1
                         AND attribution_confidence >= ?3
                         AND attribution_confidence < ?2
                     THEN 1 ELSE 0 END) AS digest_hits,
            SUM(CASE WHEN label = 1
                         AND attribution_confidence > 0.0
                         AND attribution_confidence < ?3
                     THEN 1 ELSE 0 END) AS query_only_hits,
            SUM(CASE WHEN label = 1 AND attribution_confidence <= 0.0 THEN 1 ELSE 0 END)
                AS unattributed_hits
        FROM training_examples_v1
        WHERE created_at >= ?1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return metrics;
    }

    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const qint64 cutoffSec =
        nowSec - static_cast<qint64>(std::max(1, lookbackDays)) * 24 * 60 * 60;
    sqlite3_bind_double(stmt, 1, static_cast<double>(cutoffSec));
    sqlite3_bind_double(stmt, 2, kAttributionContextThreshold);
    sqlite3_bind_double(stmt, 3, kAttributionDigestThreshold);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const int positives = sqlite3_column_int(stmt, 0);
        const int contextHits = sqlite3_column_int(stmt, 1);
        const int digestHits = sqlite3_column_int(stmt, 2);
        const int queryOnlyHits = sqlite3_column_int(stmt, 3);
        const int unattributedPositives = sqlite3_column_int(stmt, 4);
        const int attributedExamples = contextHits + digestHits + queryOnlyHits;
        metrics[QStringLiteral("positiveExamples")] = positives;
        metrics[QStringLiteral("attributedExamples")] = attributedExamples;
        metrics[QStringLiteral("contextHits")] = contextHits;
        metrics[QStringLiteral("digestHits")] = digestHits;
        metrics[QStringLiteral("queryOnlyHits")] = queryOnlyHits;
        metrics[QStringLiteral("unattributedPositives")] = unattributedPositives;
        if (positives > 0) {
            const double denom = static_cast<double>(positives);
            metrics[QStringLiteral("attributedRate")] = attributedExamples / denom;
            metrics[QStringLiteral("contextHitRate")] = contextHits / denom;
            metrics[QStringLiteral("digestHitRate")] = digestHits / denom;
            metrics[QStringLiteral("queryOnlyRate")] = queryOnlyHits / denom;
            metrics[QStringLiteral("unattributedRate")] = unattributedPositives / denom;
        }
    }

    sqlite3_finalize(stmt);
    return metrics;
}

QJsonObject collectBehaviorCoverageMetrics(sqlite3* db, int lookbackDays)
{
    QJsonObject metrics;
    metrics[QStringLiteral("windowDays")] = std::max(1, lookbackDays);
    metrics[QStringLiteral("events")] = 0;
    metrics[QStringLiteral("appBundlePresent")] = 0;
    metrics[QStringLiteral("activityDigestPresent")] = 0;
    metrics[QStringLiteral("contextEventPresent")] = 0;
    metrics[QStringLiteral("eventsWithAnyContextSignal")] = 0;
    metrics[QStringLiteral("eventsWithFullContextSignals")] = 0;
    metrics[QStringLiteral("appBundleCoverage")] = 0.0;
    metrics[QStringLiteral("activityDigestCoverage")] = 0.0;
    metrics[QStringLiteral("contextEventCoverage")] = 0.0;
    metrics[QStringLiteral("anyContextSignalCoverage")] = 0.0;
    metrics[QStringLiteral("fullContextSignalsCoverage")] = 0.0;

    if (!db) {
        return metrics;
    }

    static constexpr const char* kSql = R"(
        SELECT
            COUNT(*) AS events,
            SUM(CASE WHEN COALESCE(app_bundle_id, '') <> '' THEN 1 ELSE 0 END)
                AS app_bundle_present,
            SUM(CASE WHEN COALESCE(activity_digest, '') <> '' THEN 1 ELSE 0 END)
                AS digest_present,
            SUM(CASE WHEN COALESCE(context_event_id, '') <> '' THEN 1 ELSE 0 END)
                AS context_present,
            SUM(CASE WHEN COALESCE(app_bundle_id, '') <> ''
                          OR COALESCE(activity_digest, '') <> ''
                          OR COALESCE(context_event_id, '') <> ''
                     THEN 1 ELSE 0 END) AS any_context_signal,
            SUM(CASE WHEN COALESCE(app_bundle_id, '') <> ''
                          AND COALESCE(activity_digest, '') <> ''
                          AND COALESCE(context_event_id, '') <> ''
                     THEN 1 ELSE 0 END) AS full_context_signals
        FROM behavior_events_v1
        WHERE timestamp >= ?1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return metrics;
    }

    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const qint64 cutoffSec =
        nowSec - static_cast<qint64>(std::max(1, lookbackDays)) * 24 * 60 * 60;
    sqlite3_bind_double(stmt, 1, static_cast<double>(cutoffSec));

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const int events = sqlite3_column_int(stmt, 0);
        const int appBundlePresent = sqlite3_column_int(stmt, 1);
        const int digestPresent = sqlite3_column_int(stmt, 2);
        const int contextPresent = sqlite3_column_int(stmt, 3);
        const int anyContextSignal = sqlite3_column_int(stmt, 4);
        const int fullContextSignals = sqlite3_column_int(stmt, 5);
        metrics[QStringLiteral("events")] = events;
        metrics[QStringLiteral("appBundlePresent")] = appBundlePresent;
        metrics[QStringLiteral("activityDigestPresent")] = digestPresent;
        metrics[QStringLiteral("contextEventPresent")] = contextPresent;
        metrics[QStringLiteral("eventsWithAnyContextSignal")] = anyContextSignal;
        metrics[QStringLiteral("eventsWithFullContextSignals")] = fullContextSignals;
        if (events > 0) {
            const double denom = static_cast<double>(events);
            metrics[QStringLiteral("appBundleCoverage")] = appBundlePresent / denom;
            metrics[QStringLiteral("activityDigestCoverage")] = digestPresent / denom;
            metrics[QStringLiteral("contextEventCoverage")] = contextPresent / denom;
            metrics[QStringLiteral("anyContextSignalCoverage")] = anyContextSignal / denom;
            metrics[QStringLiteral("fullContextSignalsCoverage")] = fullContextSignals / denom;
        }
    }

    sqlite3_finalize(stmt);
    return metrics;
}

QString generateId()
{
    return QStringLiteral("%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate64(), 16, 16, QLatin1Char('0'));
}

bool directoryHasAnyEntries(const QString& path)
{
    const QDir dir(path);
    if (!dir.exists()) {
        return false;
    }
    return !dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System)
                .isEmpty();
}

bool copyPathRecursively(const QString& sourcePath, const QString& destinationPath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        return false;
    }

    if (sourceInfo.isDir()) {
        if (!QDir().mkpath(destinationPath)) {
            return false;
        }
        const QDir sourceDir(sourcePath);
        const QFileInfoList entries = sourceDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        for (const QFileInfo& entry : entries) {
            const QString childDestination =
                QDir(destinationPath).filePath(entry.fileName());
            if (!copyPathRecursively(entry.filePath(), childDestination)) {
                return false;
            }
        }
        return true;
    }

    if (sourceInfo.isFile()) {
        if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
            return false;
        }
        QFile::remove(destinationPath);
        return QFile::copy(sourcePath, destinationPath);
    }

    return false;
}

QStringList coreMlBootstrapCandidates()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList candidates;

    const QString envOverride =
        env.value(QStringLiteral("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR")).trimmed();
    if (!envOverride.isEmpty()) {
        candidates << QDir::cleanPath(envOverride);
    }

    const QString envModelsDir =
        env.value(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR")).trimmed();
    if (!envModelsDir.isEmpty()) {
        candidates << QDir::cleanPath(
            QDir(envModelsDir).filePath(QStringLiteral("online-ranker-v1/bootstrap")));
    }

    candidates << QDir::cleanPath(
        appDir + QStringLiteral("/../Resources/models/online-ranker-v1/bootstrap"));
    candidates << QDir::cleanPath(
        appDir + QStringLiteral("/../../app/betterspotlight.app/Contents/Resources/models/online-ranker-v1/bootstrap"));
    candidates << QDir::cleanPath(
        appDir + QStringLiteral("/../../../app/betterspotlight.app/Contents/Resources/models/online-ranker-v1/bootstrap"));
    candidates << QDir::cleanPath(
        appDir + QStringLiteral("/../../../../data/models/online-ranker-v1/bootstrap"));

#ifdef BETTERSPOTLIGHT_SOURCE_DIR
    candidates << QDir::cleanPath(QString::fromUtf8(BETTERSPOTLIGHT_SOURCE_DIR)
                                  + QStringLiteral("/data/models/online-ranker-v1/bootstrap"));
#endif

    candidates.removeDuplicates();
    return candidates;
}

bool ensureCoreMlBootstrapSeeded(const QString& modelRootDir, QString* seededFrom = nullptr)
{
    if (seededFrom) {
        seededFrom->clear();
    }

    const QString bootstrapDir = QDir(modelRootDir).filePath(QStringLiteral("bootstrap"));
    const QString destinationModelDir =
        QDir(bootstrapDir).filePath(QStringLiteral("online_ranker_v1.mlmodelc"));
    const QString destinationMetadataPath =
        QDir(bootstrapDir).filePath(QStringLiteral("metadata.json"));
    if (directoryHasAnyEntries(destinationModelDir)) {
        return true;
    }

    const QStringList candidates = coreMlBootstrapCandidates();
    for (const QString& candidate : candidates) {
        const QString candidateModelDir =
            QDir(candidate).filePath(QStringLiteral("online_ranker_v1.mlmodelc"));
        if (!directoryHasAnyEntries(candidateModelDir)) {
            continue;
        }
        if (!copyPathRecursively(candidateModelDir, destinationModelDir)) {
            continue;
        }

        const QString candidateMetadataPath = QDir(candidate).filePath(QStringLiteral("metadata.json"));
        if (QFileInfo::exists(candidateMetadataPath)) {
            copyPathRecursively(candidateMetadataPath, destinationMetadataPath);
        }

        if (seededFrom) {
            *seededFrom = candidate;
        }
        return true;
    }
    return false;
}

} // namespace

LearningEngine::LearningEngine(sqlite3* db, QString dataDir)
    : m_db(db)
    , m_dataDir(std::move(dataDir))
{
    m_modelRootDir = QDir(m_dataDir).filePath(QStringLiteral("models/online-ranker-v1"));
    const QString baseDir = QDir(m_modelRootDir).filePath(QStringLiteral("active"));
    m_modelPath = QDir(baseDir).filePath(QStringLiteral("weights.json"));
    m_coreMlRanker = std::make_unique<CoreMlRanker>(m_modelRootDir);
    m_ranker = std::make_unique<OnlineRanker>(m_modelPath);
}

LearningEngine::~LearningEngine() = default;

bool LearningEngine::initialize()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || !m_ranker) {
        return false;
    }

    QDir().mkpath(QFileInfo(m_modelPath).absolutePath());
    m_ranker->load();
    if (m_coreMlRanker) {
        QString seededFrom;
        if (ensureCoreMlBootstrapSeeded(m_modelRootDir, &seededFrom) && !seededFrom.isEmpty()) {
            LOG_INFO(bsCore,
                     "LearningEngine: seeded CoreML online ranker bootstrap from %s",
                     qPrintable(seededFrom));
        }
        QString coreMlError;
        const bool coreMlReady = m_coreMlRanker->initialize(&coreMlError);
        setSetting(QStringLiteral("onlineRankerCoreMlReady"), coreMlReady ? QStringLiteral("1")
                                                                          : QStringLiteral("0"));
        if (!coreMlReady && !coreMlError.isEmpty()) {
            setSetting(QStringLiteral("onlineRankerCoreMlInitError"), coreMlError);
        } else if (coreMlReady) {
            setSetting(QStringLiteral("onlineRankerCoreMlInitError"), QString());
        }
    }

    m_replaySeenCount = static_cast<uint64_t>(getSetting(
        QStringLiteral("onlineRankerReplaySeenCount"),
        QStringLiteral("0")).toULongLong());

    m_lastCycleStatus = getSetting(QStringLiteral("onlineRankerLastCycleStatus"),
                                   QStringLiteral("never_run"));
    m_lastCycleReason = getSetting(QStringLiteral("onlineRankerLastCycleReason"), QString());
    m_lastActiveLoss = getSettingDouble(QStringLiteral("onlineRankerLastActiveLoss"), 0.0);
    m_lastCandidateLoss = getSettingDouble(QStringLiteral("onlineRankerLastCandidateLoss"), 0.0);
    m_lastActiveLatencyUs = getSettingDouble(QStringLiteral("onlineRankerLastActiveLatencyUs"), 0.0);
    m_lastCandidateLatencyUs = getSettingDouble(QStringLiteral("onlineRankerLastCandidateLatencyUs"), 0.0);
    m_lastActiveFailureRate = getSettingDouble(
        QStringLiteral("onlineRankerLastActivePredictionFailureRate"),
        0.0);
    m_lastCandidateFailureRate = getSettingDouble(
        QStringLiteral("onlineRankerLastCandidatePredictionFailureRate"),
        0.0);
    m_lastActiveSaturationRate = getSettingDouble(QStringLiteral("onlineRankerLastActiveSaturationRate"), 0.0);
    m_lastCandidateSaturationRate = getSettingDouble(
        QStringLiteral("onlineRankerLastCandidateSaturationRate"),
        0.0);
    m_lastSampleCount = getSettingInt(QStringLiteral("onlineRankerLastSampleCount"), 0);
    m_lastPromoted = getSettingBool(QStringLiteral("onlineRankerLastPromoted"), false);
    m_lastManual = getSettingBool(QStringLiteral("onlineRankerLastManual"), false);
    m_lastCycleAtMs = static_cast<qint64>(getSetting(
        QStringLiteral("onlineRankerLastCycleAtMs"),
        QStringLiteral("0")).toLongLong());
    m_cyclesRun = getSetting(QStringLiteral("onlineRankerCyclesRun"), QStringLiteral("0")).toInt();
    m_cyclesSucceeded = getSetting(QStringLiteral("onlineRankerCyclesSucceeded"), QStringLiteral("0")).toInt();
    m_cyclesRejected = getSetting(QStringLiteral("onlineRankerCyclesRejected"), QStringLiteral("0")).toInt();
    m_fallbackMissingModel = getSetting(
        QStringLiteral("onlineRankerFallbackMissingModel"),
        QStringLiteral("0")).toInt();
    m_fallbackLearningDisabled = getSetting(
        QStringLiteral("onlineRankerFallbackLearningDisabled"),
        QStringLiteral("0")).toInt();
    m_fallbackResourceBudget = getSetting(
        QStringLiteral("onlineRankerFallbackResourceBudget"),
        QStringLiteral("0")).toInt();
    m_fallbackRolloutMode = getSetting(
        QStringLiteral("onlineRankerFallbackRolloutMode"),
        QStringLiteral("0")).toInt();
    m_lastPruneAtMs = static_cast<qint64>(getSetting(
        QStringLiteral("onlineRankerLastPruneAtMs"),
        QStringLiteral("0")).toLongLong());

    bool rolloutValid = false;
    const QString rolloutMode = canonicalRolloutMode(
        getSetting(QStringLiteral("onlineRankerRolloutMode"),
                   QString::fromLatin1(kRolloutInstrumentationOnly)),
        &rolloutValid);
    if (!rolloutValid
        || getSetting(QStringLiteral("onlineRankerRolloutMode"), QString()) != rolloutMode) {
        setSetting(QStringLiteral("onlineRankerRolloutMode"), rolloutMode);
    }

    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void LearningEngine::noteUserActivity()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();
}

bool LearningEngine::setSetting(const QString& key, const QString& value) const
{
    if (!m_db) {
        return false;
    }

    static constexpr const char* kSql = R"(
        INSERT INTO settings (key, value) VALUES (?1, ?2)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, valueUtf8.constData(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

QString LearningEngine::getSetting(const QString& key, const QString& fallback) const
{
    if (!m_db) {
        return fallback;
    }

    static constexpr const char* kSql =
        "SELECT value FROM settings WHERE key = ?1 LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return fallback;
    }

    const QByteArray keyUtf8 = key.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);

    QString value = fallback;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        value = raw ? QString::fromUtf8(raw) : fallback;
    }
    sqlite3_finalize(stmt);
    return value;
}

bool LearningEngine::getSettingBool(const QString& key, bool fallback) const
{
    const QString raw = getSetting(key, fallback ? QStringLiteral("1") : QStringLiteral("0"));
    const QString normalized = raw.trimmed().toLower();
    if (normalized.isEmpty()) {
        return fallback;
    }
    return normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on");
}

int LearningEngine::getSettingInt(const QString& key, int fallback) const
{
    bool ok = false;
    const int parsed = getSetting(key, QString::number(fallback)).toInt(&ok);
    return ok ? parsed : fallback;
}

double LearningEngine::getSettingDouble(const QString& key, double fallback) const
{
    bool ok = false;
    const double parsed = getSetting(key, QString::number(fallback, 'g', 12)).toDouble(&ok);
    return ok ? parsed : fallback;
}

bool LearningEngine::setModelState(const QString& key, const QString& value) const
{
    if (!m_db) {
        return false;
    }

    static constexpr const char* kSql = R"(
        INSERT INTO learning_model_state_v1 (key, value) VALUES (?1, ?2)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, valueUtf8.constData(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

QSet<QString> LearningEngine::readDenylistApps() const
{
    QSet<QString> denylist;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(
        getSetting(QStringLiteral("learningDenylistApps"), QStringLiteral("[]")).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return denylist;
    }
    for (const QJsonValue& value : doc.array()) {
        const QString normalized = value.toString().trimmed().toLower();
        if (!normalized.isEmpty()) {
            denylist.insert(normalized);
        }
    }
    return denylist;
}

void LearningEngine::maybePruneExpiredDataUnlocked()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if ((nowMs - m_lastPruneAtMs) < kPruneIntervalMs) {
        return;
    }
    m_lastPruneAtMs = nowMs;
    setSetting(QStringLiteral("onlineRankerLastPruneAtMs"), QString::number(m_lastPruneAtMs));

    const int retentionDays = std::max(
        1, getSettingInt(QStringLiteral("behaviorRawRetentionDays"), 30));
    const double cutoffSec = static_cast<double>(QDateTime::currentSecsSinceEpoch()
                                                 - static_cast<qint64>(retentionDays) * 24 * 60 * 60);
    const QString sql = QStringLiteral(
        "DELETE FROM behavior_events_v1 WHERE created_at < %1")
                            .arg(QString::number(cutoffSec, 'f', 3));
    execSql(m_db, sql.toUtf8().constData());
}

bool LearningEngine::passesResourceBudgetsUnlocked(QString* reasonOut) const
{
    if (reasonOut) {
        reasonOut->clear();
    }

    const int cpuMaxPct = std::max(1, getSettingInt(QStringLiteral("learningIdleCpuPctMax"), 35));
    const int memMaxMb = std::max(64, getSettingInt(QStringLiteral("learningMemMbMax"), 256));
    const int thermalMax = std::max(0, getSettingInt(QStringLiteral("learningThermalMax"), 2));

    const double cpuPct = currentProcessCpuPct();
    if (cpuPct >= 0.0 && cpuPct > static_cast<double>(cpuMaxPct)) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("cpu_budget_exceeded");
        }
        return false;
    }

    const double rssMb = currentProcessRssMb();
    if (rssMb >= 0.0 && rssMb > static_cast<double>(memMaxMb)) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("memory_budget_exceeded");
        }
        return false;
    }

    const int thermalState = currentThermalState();
    if (thermalState >= 0 && thermalState > thermalMax) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("thermal_budget_exceeded");
        }
        return false;
    }

    return true;
}

bool LearningEngine::recordBehaviorEvent(const BehaviorEvent& event,
                                         QString* errorOut,
                                         bool* persistedOut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (errorOut) {
        errorOut->clear();
    }
    if (persistedOut) {
        *persistedOut = false;
    }

    if (!m_db) {
        if (errorOut) {
            *errorOut = QStringLiteral("learning_db_unavailable");
        }
        return false;
    }

    if (!getSettingBool(QStringLiteral("behaviorStreamEnabled"), false)) {
        return true;
    }

    const QString source = event.source.trimmed().toLower();
    const QString eventType = event.eventType.trimmed().toLower();
    const bool captureAppActivityEnabled = getSettingBool(
        QStringLiteral("behaviorCaptureAppActivityEnabled"), true);
    const bool captureInputActivityEnabled = getSettingBool(
        QStringLiteral("behaviorCaptureInputActivityEnabled"), true);
    const bool captureSearchEventsEnabled = getSettingBool(
        QStringLiteral("behaviorCaptureSearchEventsEnabled"), true);
    const bool captureWindowTitleHashEnabled = getSettingBool(
        QStringLiteral("behaviorCaptureWindowTitleHashEnabled"), true);
    const bool captureBrowserHostHashEnabled = getSettingBool(
        QStringLiteral("behaviorCaptureBrowserHostHashEnabled"), true);

    if (eventType == QLatin1String("app_activated") && !captureAppActivityEnabled) {
        return true;
    }
    if (eventType == QLatin1String("input_activity") && !captureInputActivityEnabled) {
        return true;
    }
    if (source == QLatin1String("betterspotlight")
        && (eventType == QLatin1String("query_submitted")
            || eventType == QLatin1String("result_open")
            || eventType == QLatin1String("result_select")
            || eventType == QLatin1String("result_activate"))
        && !captureSearchEventsEnabled) {
        return true;
    }

    maybePruneExpiredDataUnlocked();

    const QString appBundleId = event.appBundleId.trimmed().toLower();
    if (!appBundleId.isEmpty() && readDenylistApps().contains(appBundleId)) {
        return true;
    }

    if (event.privacyFlags.secureInput || event.privacyFlags.privateContext
        || event.privacyFlags.denylistedApp || event.privacyFlags.redacted) {
        return true;
    }

    QString windowTitleHash = event.windowTitleHash;
    QString browserHostHash = event.browserHostHash;
    if (!captureWindowTitleHashEnabled) {
        windowTitleHash.clear();
    }
    if (!captureBrowserHostHashEnabled) {
        browserHostHash.clear();
    }

    m_lastUserActivityMs = QDateTime::currentMSecsSinceEpoch();

    static constexpr const char* kSql = R"(
        INSERT OR IGNORE INTO behavior_events_v1 (
            event_id,
            timestamp,
            source,
            event_type,
            app_bundle_id,
            window_title_hash,
            item_path,
            item_id,
            browser_host_hash,
            input_meta,
            mouse_meta,
            privacy_flags,
            attribution_confidence,
            context_event_id,
            activity_digest,
            created_at
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (errorOut) {
            *errorOut = QStringLiteral("prepare_behavior_insert_failed");
        }
        return false;
    }

    const QString eventId = event.eventId.trimmed().isEmpty() ? generateId() : event.eventId.trimmed();

    QJsonObject inputMeta;
    inputMeta[QStringLiteral("keyEventCount")] = event.inputMeta.keyEventCount;
    inputMeta[QStringLiteral("shortcutCount")] = event.inputMeta.shortcutCount;
    inputMeta[QStringLiteral("scrollCount")] = event.inputMeta.scrollCount;
    inputMeta[QStringLiteral("metadataOnly")] = event.inputMeta.metadataOnly;

    QJsonObject mouseMeta;
    mouseMeta[QStringLiteral("moveDistancePx")] = event.mouseMeta.moveDistancePx;
    mouseMeta[QStringLiteral("clickCount")] = event.mouseMeta.clickCount;
    mouseMeta[QStringLiteral("dragCount")] = event.mouseMeta.dragCount;

    QJsonObject privacyFlags;
    privacyFlags[QStringLiteral("secureInput")] = event.privacyFlags.secureInput;
    privacyFlags[QStringLiteral("privateContext")] = event.privacyFlags.privateContext;
    privacyFlags[QStringLiteral("denylistedApp")] = event.privacyFlags.denylistedApp;
    privacyFlags[QStringLiteral("redacted")] = event.privacyFlags.redacted;

    const QByteArray eventIdUtf8 = eventId.toUtf8();
    const QByteArray sourceUtf8 = event.source.trimmed().isEmpty()
        ? QByteArrayLiteral("betterspotlight")
        : event.source.toUtf8();
    const QByteArray eventTypeUtf8 = event.eventType.trimmed().isEmpty()
        ? QByteArrayLiteral("activity")
        : event.eventType.toUtf8();
    const QByteArray appUtf8 = event.appBundleId.toUtf8();
    const QByteArray windowUtf8 = windowTitleHash.toUtf8();
    const QByteArray pathUtf8 = event.itemPath.toUtf8();
    const QByteArray hostUtf8 = browserHostHash.toUtf8();
    const QByteArray inputUtf8 = QJsonDocument(inputMeta).toJson(QJsonDocument::Compact);
    const QByteArray mouseUtf8 = QJsonDocument(mouseMeta).toJson(QJsonDocument::Compact);
    const QByteArray privacyUtf8 = QJsonDocument(privacyFlags).toJson(QJsonDocument::Compact);
    const QByteArray ctxEventUtf8 = event.contextEventId.toUtf8();
    const QByteArray digestUtf8 = event.activityDigest.toUtf8();
    const double nowSec = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const double tsSec = event.timestamp.isValid()
        ? static_cast<double>(event.timestamp.toUTC().toSecsSinceEpoch())
        : nowSec;

    sqlite3_bind_text(stmt, 1, eventIdUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, tsSec);
    sqlite3_bind_text(stmt, 3, sourceUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, eventTypeUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (event.appBundleId.isEmpty()) sqlite3_bind_null(stmt, 5);
    else sqlite3_bind_text(stmt, 5, appUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (windowTitleHash.isEmpty()) sqlite3_bind_null(stmt, 6);
    else sqlite3_bind_text(stmt, 6, windowUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (event.itemPath.isEmpty()) sqlite3_bind_null(stmt, 7);
    else sqlite3_bind_text(stmt, 7, pathUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (event.itemId <= 0) sqlite3_bind_null(stmt, 8);
    else sqlite3_bind_int64(stmt, 8, event.itemId);
    if (browserHostHash.isEmpty()) sqlite3_bind_null(stmt, 9);
    else sqlite3_bind_text(stmt, 9, hostUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, inputUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, mouseUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, privacyUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 13, std::clamp(event.attributionConfidence, 0.0, 1.0));
    if (event.contextEventId.isEmpty()) sqlite3_bind_null(stmt, 14);
    else sqlite3_bind_text(stmt, 14, ctxEventUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (event.activityDigest.isEmpty()) sqlite3_bind_null(stmt, 15);
    else sqlite3_bind_text(stmt, 15, digestUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 16, nowSec);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const bool inserted = ok && sqlite3_changes(m_db) > 0;
    sqlite3_finalize(stmt);
    if (!ok && errorOut) {
        *errorOut = QStringLiteral("insert_behavior_event_failed");
    }
    if (ok && persistedOut) {
        *persistedOut = inserted;
    }
    return ok;
}

bool LearningEngine::setConsent(bool behaviorStreamEnabled,
                                bool learningEnabled,
                                bool learningPauseOnUserInput,
                                const QStringList& denylistApps,
                                QString* errorOut,
                                const QString& rolloutMode,
                                bool captureAppActivityEnabled,
                                bool captureInputActivityEnabled,
                                bool captureSearchEventsEnabled,
                                bool captureWindowTitleHashEnabled,
                                bool captureBrowserHostHashEnabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (errorOut) {
        errorOut->clear();
    }

    if (!m_db) {
        if (errorOut) {
            *errorOut = QStringLiteral("learning_db_unavailable");
        }
        return false;
    }

    QJsonArray denylist;
    for (const QString& app : denylistApps) {
        const QString normalized = app.trimmed().toLower();
        if (!normalized.isEmpty()) {
            denylist.append(normalized);
        }
    }

    QString rolloutSetting = getSetting(QStringLiteral("onlineRankerRolloutMode"),
                                        QString::fromLatin1(kRolloutInstrumentationOnly));
    if (!rolloutMode.trimmed().isEmpty()) {
        bool rolloutValid = false;
        rolloutSetting = canonicalRolloutMode(rolloutMode, &rolloutValid);
        if (!rolloutValid) {
            if (errorOut) {
                *errorOut = QStringLiteral("invalid_rollout_mode");
            }
            return false;
        }
    } else {
        rolloutSetting = canonicalRolloutMode(rolloutSetting, nullptr);
    }

    if (!setSetting(QStringLiteral("behaviorStreamEnabled"), behaviorStreamEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("learningEnabled"), learningEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("learningPauseOnUserInput"), learningPauseOnUserInput ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("behaviorCaptureAppActivityEnabled"),
                       captureAppActivityEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("behaviorCaptureInputActivityEnabled"),
                       captureInputActivityEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("behaviorCaptureSearchEventsEnabled"),
                       captureSearchEventsEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("behaviorCaptureWindowTitleHashEnabled"),
                       captureWindowTitleHashEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("behaviorCaptureBrowserHostHashEnabled"),
                       captureBrowserHostHashEnabled ? QStringLiteral("1") : QStringLiteral("0"))
        || !setSetting(QStringLiteral("onlineRankerRolloutMode"), rolloutSetting)
        || !setSetting(QStringLiteral("learningDenylistApps"),
                       QString::fromUtf8(QJsonDocument(denylist).toJson(QJsonDocument::Compact)))) {
        if (errorOut) {
            *errorOut = QStringLiteral("persist_consent_failed");
        }
        return false;
    }

    return true;
}

QVector<double> LearningEngine::buildFeatureVector(const SearchResult& result,
                                                   const QueryContext& context,
                                                   QueryClass queryClass,
                                                   float routerConfidence,
                                                   float semanticNeed,
                                                   int rank,
                                                   int queryTokenCount) const
{
    ContextFeatureVector contextFeatures;
    contextFeatures.version = context.contextFeatureVersion.value_or(1);
    contextFeatures.contextEventId = context.contextEventId.value_or(QString());
    contextFeatures.activityDigest = context.activityDigest.value_or(QString());
    contextFeatures.appFocusMatch =
        (context.frontmostAppBundleId.has_value() && !context.frontmostAppBundleId->isEmpty())
            ? 1.0
            : 0.0;
    contextFeatures.keyboardActivity = 0.0;
    contextFeatures.mouseActivity = 0.0;
    contextFeatures.queryLength = std::clamp(static_cast<double>(queryTokenCount) / 8.0, 0.0, 2.0);
    contextFeatures.resultRank = 1.0 / static_cast<double>(std::max(1, rank + 1));
    contextFeatures.routerConfidence = std::clamp(static_cast<double>(routerConfidence), 0.0, 1.0);
    contextFeatures.semanticNeed = std::clamp(static_cast<double>(semanticNeed), 0.0, 1.0);

    QVector<double> features;
    features.fill(0.0, kFeatureDim);

    features[0] = std::clamp(result.semanticNormalized, 0.0, 1.0);
    features[1] = std::clamp(static_cast<double>(result.crossEncoderScore), 0.0, 1.0);
    features[2] = std::clamp(result.scoreBreakdown.feedbackBoost / 25.0, 0.0, 2.0);
    features[3] = std::clamp(result.scoreBreakdown.frequencyBoost / 30.0, 0.0, 2.0);
    features[4] = std::clamp(result.scoreBreakdown.contextBoost / 25.0, -2.0, 2.0);
    features[5] = contextFeatures.semanticNeed;
    features[6] = contextFeatures.routerConfidence;
    features[7] = queryClass == QueryClass::PathOrCode ? 1.0 : 0.0;
    features[8] = queryClass == QueryClass::NaturalLanguage ? 1.0 : 0.0;
    features[9] = queryClass == QueryClass::ShortAmbiguous ? 1.0 : 0.0;
    features[10] = contextFeatures.resultRank;
    features[11] = contextFeatures.queryLength;
    features[12] = std::tanh(result.score / 300.0);

    if (contextFeatures.appFocusMatch > 0.0) {
        features[4] += 0.1;
    }

    return features;
}

QString LearningEngine::featuresToJson(const QVector<double>& features)
{
    QJsonArray arr;
    for (double value : features) {
        arr.append(value);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<double> LearningEngine::featuresFromJson(const QString& encoded)
{
    QVector<double> features;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(encoded.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return features;
    }
    const QJsonArray arr = doc.array();
    features.reserve(arr.size());
    for (const QJsonValue& val : arr) {
        features.push_back(val.toDouble(0.0));
    }
    return features;
}

bool LearningEngine::recordExposure(const QString& query,
                                    const SearchResult& result,
                                    const QueryContext& context,
                                    QueryClass queryClass,
                                    float routerConfidence,
                                    float semanticNeed,
                                    int rank,
                                    QString* errorOut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (errorOut) {
        errorOut->clear();
    }

    if (!m_db) {
        if (errorOut) {
            *errorOut = QStringLiteral("learning_db_unavailable");
        }
        return false;
    }

    if (!getSettingBool(QStringLiteral("learningEnabled"), false)
        || !getSettingBool(QStringLiteral("behaviorStreamEnabled"), false)) {
        return true;
    }

    static constexpr const char* kSql = R"(
        INSERT INTO training_examples_v1 (
            sample_id,
            created_at,
            query,
            query_normalized,
            item_id,
            path,
            label,
            weight,
            features_json,
            app_bundle_id,
            context_event_id,
            activity_digest,
            attribution_confidence,
            consumed
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, NULL, ?7, ?8, ?9, ?10, ?11, ?12, 0)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (errorOut) {
            *errorOut = QStringLiteral("prepare_exposure_insert_failed");
        }
        return false;
    }

    const QString normalizedQuery = InteractionTracker::normalizeQuery(query);
    const int queryTokenCount = std::max(
        1, static_cast<int>(normalizedQuery.split(' ', Qt::SkipEmptyParts).size()));
    const QVector<double> features = buildFeatureVector(result,
                                                        context,
                                                        queryClass,
                                                        routerConfidence,
                                                        semanticNeed,
                                                        rank,
                                                        queryTokenCount);

    const QString sampleId = generateId();
    const double nowSec = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const double weight = exposureBiasWeightForRank(rank);

    const QByteArray sampleUtf8 = sampleId.toUtf8();
    const QByteArray queryUtf8 = query.toUtf8();
    const QByteArray queryNormUtf8 = normalizedQuery.toUtf8();
    const QByteArray pathUtf8 = result.path.toUtf8();
    const QByteArray featureUtf8 = featuresToJson(features).toUtf8();
    const QByteArray appUtf8 = context.frontmostAppBundleId.has_value()
        ? context.frontmostAppBundleId->toUtf8()
        : QByteArray();
    const QByteArray contextEventUtf8 = context.contextEventId.has_value()
        ? context.contextEventId->toUtf8()
        : QByteArray();
    const QByteArray digestUtf8 = context.activityDigest.has_value()
        ? context.activityDigest->toUtf8()
        : QByteArray();

    sqlite3_bind_text(stmt, 1, sampleUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, nowSec);
    sqlite3_bind_text(stmt, 3, queryUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, queryNormUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, result.itemId);
    sqlite3_bind_text(stmt, 6, pathUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 7, weight);
    sqlite3_bind_text(stmt, 8, featureUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (appUtf8.isEmpty()) sqlite3_bind_null(stmt, 9);
    else sqlite3_bind_text(stmt, 9, appUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (contextEventUtf8.isEmpty()) sqlite3_bind_null(stmt, 10);
    else sqlite3_bind_text(stmt, 10, contextEventUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (digestUtf8.isEmpty()) sqlite3_bind_null(stmt, 11);
    else sqlite3_bind_text(stmt, 11, digestUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 12, 0.0);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok && errorOut) {
        *errorOut = QStringLiteral("insert_exposure_failed");
    }
    return ok;
}

bool LearningEngine::recordPositiveInteraction(const QString& query,
                                               int64_t itemId,
                                               const QString& path,
                                               const QString& appBundleId,
                                               const QString& contextEventId,
                                               const QString& activityDigest,
                                               const QDateTime& timestamp,
                                               QString* errorOut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (errorOut) {
        errorOut->clear();
    }

    if (!m_db) {
        if (errorOut) {
            *errorOut = QStringLiteral("learning_db_unavailable");
        }
        return false;
    }

    if (!getSettingBool(QStringLiteral("learningEnabled"), false)
        || !getSettingBool(QStringLiteral("behaviorStreamEnabled"), false)) {
        return true;
    }

    const QString normalizedQuery = InteractionTracker::normalizeQuery(query);
    const qint64 ts = timestamp.isValid()
        ? timestamp.toUTC().toSecsSinceEpoch()
        : QDateTime::currentSecsSinceEpoch();
    const qint64 fromTs = ts - 30;
    const qint64 toTs = ts + 1;
    const QString normalizedContextEventId = contextEventId.trimmed();
    const QString normalizedActivityDigest = activityDigest.trimmed();
    const QByteArray queryNormUtf8 = normalizedQuery.toUtf8();

    static constexpr const char* kContextUpdateSql = R"(
        UPDATE training_examples_v1
        SET label = 1,
            attribution_confidence = MAX(attribution_confidence, 1.0)
        WHERE item_id = ?1
          AND context_event_id = ?2
          AND consumed = 0
          AND (label IS NULL OR label < 0)
          AND created_at BETWEEN ?3 AND ?4
    )";

    if (!normalizedContextEventId.isEmpty()) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, kContextUpdateSql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (errorOut) {
                *errorOut = QStringLiteral("prepare_positive_context_update_failed");
            }
            return false;
        }
        const QByteArray contextUtf8 = normalizedContextEventId.toUtf8();
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_bind_text(stmt, 2, contextUtf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, static_cast<double>(fromTs));
        sqlite3_bind_double(stmt, 4, static_cast<double>(toTs));

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            if (errorOut) {
                *errorOut = QStringLiteral("update_positive_context_failed");
            }
            return false;
        }
        if (sqlite3_changes(m_db) > 0) {
            return true;
        }
    }

    static constexpr const char* kDigestUpdateSql = R"(
        UPDATE training_examples_v1
        SET label = 1,
            attribution_confidence = MAX(attribution_confidence, 0.85)
        WHERE item_id = ?1
          AND activity_digest = ?2
          AND query_normalized = ?3
          AND consumed = 0
          AND (label IS NULL OR label < 0)
          AND created_at BETWEEN ?4 AND ?5
    )";

    if (!normalizedActivityDigest.isEmpty() && !normalizedQuery.isEmpty()) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, kDigestUpdateSql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (errorOut) {
                *errorOut = QStringLiteral("prepare_positive_digest_update_failed");
            }
            return false;
        }
        const QByteArray digestUtf8 = normalizedActivityDigest.toUtf8();
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_bind_text(stmt, 2, digestUtf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, queryNormUtf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, static_cast<double>(fromTs));
        sqlite3_bind_double(stmt, 5, static_cast<double>(toTs));

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            if (errorOut) {
                *errorOut = QStringLiteral("update_positive_digest_failed");
            }
            return false;
        }
        if (sqlite3_changes(m_db) > 0) {
            return true;
        }
    }

    static constexpr const char* kQueryUpdateSql = R"(
        UPDATE training_examples_v1
        SET label = 1,
            attribution_confidence = MAX(attribution_confidence, 0.7)
        WHERE item_id = ?1
          AND query_normalized = ?2
          AND consumed = 0
          AND (label IS NULL OR label < 0)
          AND created_at BETWEEN ?3 AND ?4
    )";

    if (!normalizedQuery.isEmpty()) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, kQueryUpdateSql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (errorOut) {
                *errorOut = QStringLiteral("prepare_positive_query_update_failed");
            }
            return false;
        }
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_bind_text(stmt, 2, queryNormUtf8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, static_cast<double>(fromTs));
        sqlite3_bind_double(stmt, 4, static_cast<double>(toTs));

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            if (errorOut) {
                *errorOut = QStringLiteral("update_positive_query_failed");
            }
            return false;
        }
        if (sqlite3_changes(m_db) > 0) {
            return true;
        }
    }

    static constexpr const char* kInsertSql = R"(
        INSERT INTO training_examples_v1 (
            sample_id,
            created_at,
            query,
            query_normalized,
            item_id,
            path,
            label,
            weight,
            features_json,
            app_bundle_id,
            context_event_id,
            activity_digest,
            attribution_confidence,
            consumed
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 1, 1.0, ?7, ?8, ?9, ?10, ?11, 0)
    )";

    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kInsertSql, -1, &insertStmt, nullptr) != SQLITE_OK) {
        if (errorOut) {
            *errorOut = QStringLiteral("prepare_positive_insert_failed");
        }
        return false;
    }

    QVector<double> fallbackFeatures;
    fallbackFeatures.fill(0.0, kFeatureDim);
    fallbackFeatures[10] = 1.0;
    fallbackFeatures[11] = std::clamp(
        static_cast<double>(normalizedQuery.split(' ', Qt::SkipEmptyParts).size()) / 8.0,
        0.0,
        2.0);

    const QByteArray sampleUtf8 = generateId().toUtf8();
    const QByteArray queryUtf8 = query.toUtf8();
    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray featureUtf8 = featuresToJson(fallbackFeatures).toUtf8();
    const QByteArray appUtf8 = appBundleId.toUtf8();
    const QByteArray contextUtf8 = normalizedContextEventId.toUtf8();
    const QByteArray digestUtf8 = normalizedActivityDigest.toUtf8();
    const double fallbackAttributionConfidence = !normalizedContextEventId.isEmpty()
        ? 1.0
        : (!normalizedActivityDigest.isEmpty() ? 0.85 : 0.7);

    sqlite3_bind_text(insertStmt, 1, sampleUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(insertStmt, 2, static_cast<double>(ts));
    sqlite3_bind_text(insertStmt, 3, queryUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 4, queryNormUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insertStmt, 5, itemId);
    sqlite3_bind_text(insertStmt, 6, pathUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 7, featureUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (appUtf8.isEmpty()) sqlite3_bind_null(insertStmt, 8);
    else sqlite3_bind_text(insertStmt, 8, appUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (contextUtf8.isEmpty()) sqlite3_bind_null(insertStmt, 9);
    else sqlite3_bind_text(insertStmt, 9, contextUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (digestUtf8.isEmpty()) sqlite3_bind_null(insertStmt, 10);
    else sqlite3_bind_text(insertStmt, 10, digestUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(insertStmt, 11, fallbackAttributionConfidence);

    const bool ok = sqlite3_step(insertStmt) == SQLITE_DONE;
    sqlite3_finalize(insertStmt);
    if (!ok && errorOut) {
        *errorOut = QStringLiteral("insert_positive_fallback_failed");
    }
    return ok;
}

QVector<TrainingExample> LearningEngine::fetchFreshExamplesForTraining(
    int limit,
    int staleNegativeSeconds,
    QVector<int64_t>* consumedIdsOut,
    QString* errorOut)
{
    QVector<TrainingExample> out;
    if (consumedIdsOut) {
        consumedIdsOut->clear();
    }

    static constexpr const char* kSql = R"(
        SELECT id,
               sample_id,
               created_at,
               query,
               query_normalized,
               item_id,
               path,
               label,
               weight,
               features_json,
               source_event_id,
               app_bundle_id,
               context_event_id,
               activity_digest,
               attribution_confidence,
               consumed
        FROM training_examples_v1
        WHERE consumed = 0
          AND (
                label = 1
             OR (label IS NULL AND created_at <= ?1)
             OR label = 0
          )
        ORDER BY created_at ASC
        LIMIT ?2
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (errorOut) {
            *errorOut = QStringLiteral("prepare_fetch_examples_failed");
        }
        return out;
    }

    const double staleCutoff = static_cast<double>(QDateTime::currentSecsSinceEpoch()
        - std::max(1, staleNegativeSeconds));
    sqlite3_bind_double(stmt, 1, staleCutoff);
    sqlite3_bind_int(stmt, 2, std::max(1, limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TrainingExample example;
        example.sampleId = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        example.createdAt = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(sqlite3_column_double(stmt, 2)), QTimeZone::UTC);
        const char* rawQuery = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* rawNorm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* rawSourceEvent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        const char* rawApp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        const char* rawCtx = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        const char* rawDigest = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));

        example.query = rawQuery ? QString::fromUtf8(rawQuery) : QString();
        example.queryNormalized = rawNorm ? QString::fromUtf8(rawNorm) : QString();
        example.itemId = sqlite3_column_int64(stmt, 5);
        example.path = rawPath ? QString::fromUtf8(rawPath) : QString();
        if (sqlite3_column_type(stmt, 7) == SQLITE_NULL) {
            example.label = 0;
        } else {
            example.label = sqlite3_column_int(stmt, 7);
        }
        example.weight = sqlite3_column_double(stmt, 8);
        example.denseFeatures = featuresFromJson(QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9))));
        example.sourceEventId = rawSourceEvent ? QString::fromUtf8(rawSourceEvent) : QString();
        example.appBundleId = rawApp ? QString::fromUtf8(rawApp) : QString();
        example.contextEventId = rawCtx ? QString::fromUtf8(rawCtx) : QString();
        example.activityDigest = rawDigest ? QString::fromUtf8(rawDigest) : QString();
        example.attributionConfidence = sqlite3_column_double(stmt, 14);
        example.consumed = sqlite3_column_int(stmt, 15) != 0;

        if (example.denseFeatures.isEmpty()) {
            continue;
        }

        out.push_back(std::move(example));
        if (consumedIdsOut) {
            consumedIdsOut->push_back(sqlite3_column_int64(stmt, 0));
        }
    }

    sqlite3_finalize(stmt);
    return out;
}

QVector<TrainingExample> LearningEngine::fetchReplaySamples(int limit) const
{
    QVector<TrainingExample> out;
    if (!m_db) {
        return out;
    }

    static constexpr const char* kSql = R"(
        SELECT sample_id,
               label,
               weight,
               features_json,
               query_normalized,
               item_id,
               created_at
        FROM replay_reservoir_v1
        ORDER BY slot ASC
        LIMIT ?1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(stmt, 1, std::max(1, limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TrainingExample example;
        const char* sample = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* feature = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* queryNorm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

        example.sampleId = sample ? QString::fromUtf8(sample) : generateId();
        example.label = sqlite3_column_int(stmt, 1);
        example.weight = sqlite3_column_double(stmt, 2);
        example.denseFeatures = featuresFromJson(feature ? QString::fromUtf8(feature) : QString());
        example.queryNormalized = queryNorm ? QString::fromUtf8(queryNorm) : QString();
        example.itemId = sqlite3_column_int64(stmt, 5);
        example.createdAt = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(sqlite3_column_double(stmt, 6)), QTimeZone::UTC);

        if (!example.denseFeatures.isEmpty()) {
            out.push_back(std::move(example));
        }
    }

    sqlite3_finalize(stmt);
    return out;
}

int LearningEngine::replaySize() const
{
    if (!m_db) {
        return 0;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM replay_reservoir_v1", -1, &stmt, nullptr)
        != SQLITE_OK) {
        return 0;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int LearningEngine::pendingExamples() const
{
    if (!m_db) {
        return 0;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "SELECT COUNT(*) FROM training_examples_v1 WHERE consumed = 0",
                           -1,
                           &stmt,
                           nullptr) != SQLITE_OK) {
        return 0;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool LearningEngine::addToReplayReservoir(const TrainingExample& example, QString* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!m_db || example.denseFeatures.isEmpty() || example.label < 0) {
        return false;
    }

    const int capacity = std::max(256, getSettingInt(QStringLiteral("onlineRankerReplayCapacity"),
                                                      kDefaultReplayCapacity));
    const int currentSize = replaySize();

    int slot = -1;
    if (currentSize < capacity) {
        slot = currentSize;
    } else {
        const uint64_t seen = m_replaySeenCount;
        const uint64_t draw = static_cast<uint64_t>(
            QRandomGenerator::global()->generate64() % (seen + 1));
        if (draw >= static_cast<uint64_t>(capacity)) {
            ++m_replaySeenCount;
            setSetting(QStringLiteral("onlineRankerReplaySeenCount"),
                       QString::number(m_replaySeenCount));
            return true;
        }
        slot = static_cast<int>(draw);
    }

    static constexpr const char* kUpsertSql = R"(
        INSERT INTO replay_reservoir_v1 (
            slot,
            sample_id,
            label,
            weight,
            features_json,
            query_normalized,
            item_id,
            created_at
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(slot) DO UPDATE SET
            sample_id = excluded.sample_id,
            label = excluded.label,
            weight = excluded.weight,
            features_json = excluded.features_json,
            query_normalized = excluded.query_normalized,
            item_id = excluded.item_id,
            created_at = excluded.created_at
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kUpsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (errorOut) {
            *errorOut = QStringLiteral("prepare_replay_upsert_failed");
        }
        return false;
    }

    const QByteArray sampleUtf8 = (example.sampleId.isEmpty() ? generateId() : example.sampleId).toUtf8();
    const QByteArray featuresUtf8 = featuresToJson(example.denseFeatures).toUtf8();
    const QByteArray queryNormUtf8 = example.queryNormalized.toUtf8();

    sqlite3_bind_int(stmt, 1, slot);
    sqlite3_bind_text(stmt, 2, sampleUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, example.label);
    sqlite3_bind_double(stmt, 4, std::max(0.05, example.weight));
    sqlite3_bind_text(stmt, 5, featuresUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (queryNormUtf8.isEmpty()) sqlite3_bind_null(stmt, 6);
    else sqlite3_bind_text(stmt, 6, queryNormUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, example.itemId);
    sqlite3_bind_double(stmt, 8, static_cast<double>(
        example.createdAt.isValid()
            ? example.createdAt.toUTC().toSecsSinceEpoch()
            : QDateTime::currentSecsSinceEpoch()));

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok && errorOut) {
        *errorOut = QStringLiteral("upsert_replay_sample_failed");
    }

    ++m_replaySeenCount;
    setSetting(QStringLiteral("onlineRankerReplaySeenCount"), QString::number(m_replaySeenCount));
    return ok;
}

void LearningEngine::setLastCycleResult(const QString& status,
                                        const QString& reason,
                                        double activeLoss,
                                        double candidateLoss,
                                        int sampleCount,
                                        bool promoted,
                                        bool manual)
{
    m_lastCycleStatus = status;
    m_lastCycleReason = reason;
    m_lastActiveLoss = activeLoss;
    m_lastCandidateLoss = candidateLoss;
    m_lastSampleCount = sampleCount;
    m_lastPromoted = promoted;
    m_lastManual = manual;
    m_lastCycleAtMs = QDateTime::currentMSecsSinceEpoch();

    setSetting(QStringLiteral("onlineRankerLastCycleStatus"), status);
    setSetting(QStringLiteral("onlineRankerLastCycleReason"), reason);
    setSetting(QStringLiteral("onlineRankerLastCycleAtMs"), QString::number(m_lastCycleAtMs));
    setSetting(QStringLiteral("onlineRankerLastActiveLoss"), QString::number(activeLoss, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastCandidateLoss"), QString::number(candidateLoss, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastActiveLatencyUs"),
               QString::number(m_lastActiveLatencyUs, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastCandidateLatencyUs"),
               QString::number(m_lastCandidateLatencyUs, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastActivePredictionFailureRate"),
               QString::number(m_lastActiveFailureRate, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastCandidatePredictionFailureRate"),
               QString::number(m_lastCandidateFailureRate, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastActiveSaturationRate"),
               QString::number(m_lastActiveSaturationRate, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastCandidateSaturationRate"),
               QString::number(m_lastCandidateSaturationRate, 'g', 10));
    setSetting(QStringLiteral("onlineRankerLastSampleCount"), QString::number(sampleCount));
    setSetting(QStringLiteral("onlineRankerLastPromoted"), promoted ? QStringLiteral("1") : QStringLiteral("0"));
    setSetting(QStringLiteral("onlineRankerLastManual"), manual ? QStringLiteral("1") : QStringLiteral("0"));
    setSetting(QStringLiteral("onlineRankerCyclesRun"), QString::number(m_cyclesRun));
    setSetting(QStringLiteral("onlineRankerCyclesSucceeded"), QString::number(m_cyclesSucceeded));
    setSetting(QStringLiteral("onlineRankerCyclesRejected"), QString::number(m_cyclesRejected));
    setModelState(QStringLiteral("last_cycle_status"), status);
    setModelState(QStringLiteral("last_cycle_reason"), reason);
    setModelState(QStringLiteral("last_active_loss"), QString::number(activeLoss, 'g', 10));
    setModelState(QStringLiteral("last_candidate_loss"), QString::number(candidateLoss, 'g', 10));
    setModelState(QStringLiteral("last_active_latency_us"),
                  QString::number(m_lastActiveLatencyUs, 'g', 10));
    setModelState(QStringLiteral("last_candidate_latency_us"),
                  QString::number(m_lastCandidateLatencyUs, 'g', 10));
    setModelState(QStringLiteral("last_active_prediction_failure_rate"),
                  QString::number(m_lastActiveFailureRate, 'g', 10));
    setModelState(QStringLiteral("last_candidate_prediction_failure_rate"),
                  QString::number(m_lastCandidateFailureRate, 'g', 10));
    setModelState(QStringLiteral("last_active_saturation_rate"),
                  QString::number(m_lastActiveSaturationRate, 'g', 10));
    setModelState(QStringLiteral("last_candidate_saturation_rate"),
                  QString::number(m_lastCandidateSaturationRate, 'g', 10));
    setModelState(QStringLiteral("last_sample_count"), QString::number(sampleCount));
    setModelState(QStringLiteral("last_promoted"), promoted ? QStringLiteral("1") : QStringLiteral("0"));
    setModelState(QStringLiteral("last_manual"), manual ? QStringLiteral("1") : QStringLiteral("0"));
    setModelState(QStringLiteral("last_cycle_at_ms"), QString::number(m_lastCycleAtMs));

    QJsonObject batchAttribution;
    batchAttribution[QStringLiteral("positiveExamples")] = m_lastBatchPositiveExamples;
    batchAttribution[QStringLiteral("contextHits")] = m_lastBatchContextHits;
    batchAttribution[QStringLiteral("digestHits")] = m_lastBatchDigestHits;
    batchAttribution[QStringLiteral("queryOnlyHits")] = m_lastBatchQueryOnlyHits;
    batchAttribution[QStringLiteral("unattributedPositives")] = m_lastBatchUnattributedPositives;
    batchAttribution[QStringLiteral("attributedRate")] = m_lastBatchAttributedRate;
    batchAttribution[QStringLiteral("contextRate")] = m_lastBatchContextRate;
    batchAttribution[QStringLiteral("digestRate")] = m_lastBatchDigestRate;
    batchAttribution[QStringLiteral("queryOnlyRate")] = m_lastBatchQueryOnlyRate;
    batchAttribution[QStringLiteral("unattributedRate")] = m_lastBatchUnattributedRate;
    batchAttribution[QStringLiteral("contextDigestRate")] = m_lastBatchContextDigestRate;

    QJsonObject cycleEntry;
    cycleEntry[QStringLiteral("cycleAtMs")] = m_lastCycleAtMs;
    cycleEntry[QStringLiteral("cycleIndex")] = m_cyclesRun;
    cycleEntry[QStringLiteral("status")] = status;
    cycleEntry[QStringLiteral("reason")] = reason;
    cycleEntry[QStringLiteral("activeLoss")] = activeLoss;
    cycleEntry[QStringLiteral("candidateLoss")] = candidateLoss;
    cycleEntry[QStringLiteral("activeLatencyUs")] = m_lastActiveLatencyUs;
    cycleEntry[QStringLiteral("candidateLatencyUs")] = m_lastCandidateLatencyUs;
    cycleEntry[QStringLiteral("activePredictionFailureRate")] = m_lastActiveFailureRate;
    cycleEntry[QStringLiteral("candidatePredictionFailureRate")] = m_lastCandidateFailureRate;
    cycleEntry[QStringLiteral("activeSaturationRate")] = m_lastActiveSaturationRate;
    cycleEntry[QStringLiteral("candidateSaturationRate")] = m_lastCandidateSaturationRate;
    cycleEntry[QStringLiteral("sampleCount")] = sampleCount;
    cycleEntry[QStringLiteral("promoted")] = promoted;
    cycleEntry[QStringLiteral("manual")] = manual;
    cycleEntry[QStringLiteral("rolloutMode")] = canonicalRolloutMode(
        getSetting(QStringLiteral("onlineRankerRolloutMode"),
                   QString::fromLatin1(kRolloutInstrumentationOnly)),
        nullptr);
    cycleEntry[QStringLiteral("batchAttribution")] = batchAttribution;

    const int historyLimit = std::max(
        1, getSettingInt(QStringLiteral("onlineRankerRecentCycleHistoryLimit"),
                         kDefaultRecentCycleHistoryLimit));
    const QJsonArray previousHistory = parseJsonArrayOrEmpty(
        getSetting(QStringLiteral("onlineRankerRecentCycleHistory"), QStringLiteral("[]")));
    QJsonArray nextHistory;
    nextHistory.append(cycleEntry);
    for (const QJsonValue& value : previousHistory) {
        if (!value.isObject()) {
            continue;
        }
        if (nextHistory.size() >= historyLimit) {
            break;
        }
        nextHistory.append(value);
    }
    const QString historyJson = QString::fromUtf8(
        QJsonDocument(nextHistory).toJson(QJsonDocument::Compact));
    setSetting(QStringLiteral("onlineRankerRecentCycleHistory"), historyJson);
    setModelState(QStringLiteral("recent_cycle_history"), historyJson);
}

bool LearningEngine::triggerLearningCycle(bool manual, QString* reasonOut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (reasonOut) {
        reasonOut->clear();
    }

    if (!m_db || !m_ranker) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("learning_not_initialized");
        }
        return false;
    }

    if (m_cycleRunning) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("cycle_in_progress");
        }
        return false;
    }

    if (!getSettingBool(QStringLiteral("learningEnabled"), false)) {
        if (reasonOut) {
            *reasonOut = QStringLiteral("learning_disabled");
        }
        return false;
    }

    const QString rolloutMode = canonicalRolloutMode(
        getSetting(QStringLiteral("onlineRankerRolloutMode"),
                   QString::fromLatin1(kRolloutInstrumentationOnly)),
        nullptr);
    if (!rolloutAllowsTraining(rolloutMode)) {
        ++m_fallbackRolloutMode;
        setSetting(QStringLiteral("onlineRankerFallbackRolloutMode"),
                   QString::number(m_fallbackRolloutMode));
        if (reasonOut) {
            *reasonOut = QStringLiteral("rollout_mode_blocks_training");
        }
        return false;
    }

    maybePruneExpiredDataUnlocked();

    if (!manual) {
        QString budgetReason;
        if (!passesResourceBudgetsUnlocked(&budgetReason)) {
            m_lastActiveLatencyUs = 0.0;
            m_lastCandidateLatencyUs = 0.0;
            m_lastActiveFailureRate = 0.0;
            m_lastCandidateFailureRate = 0.0;
            m_lastActiveSaturationRate = 0.0;
            m_lastCandidateSaturationRate = 0.0;
            ++m_cyclesRun;
            ++m_cyclesRejected;
            ++m_fallbackResourceBudget;
            setSetting(QStringLiteral("onlineRankerFallbackResourceBudget"),
                       QString::number(m_fallbackResourceBudget));
            setLastCycleResult(QStringLiteral("rejected"),
                               budgetReason,
                               0.0,
                               0.0,
                               0,
                               false,
                               manual);
            if (reasonOut) {
                *reasonOut = budgetReason;
            }
            return false;
        }
    }

    m_cycleRunning = true;
    m_lastCycleStartedMs = QDateTime::currentMSecsSinceEpoch();
    const QString previousVersion = (m_coreMlRanker && m_coreMlRanker->hasModel())
        ? m_coreMlRanker->modelVersion()
        : m_ranker->modelVersion();

    QVector<int64_t> consumedIds;
    QString fetchError;
    QVector<TrainingExample> fresh = fetchFreshExamplesForTraining(
        getSettingInt(QStringLiteral("onlineRankerFreshTrainingLimit"), kDefaultFreshTrainingLimit),
        getSettingInt(QStringLiteral("onlineRankerNegativeStaleSeconds"), kDefaultNegativeStaleSeconds),
        &consumedIds,
        &fetchError);

    if (!fetchError.isEmpty()) {
        m_cycleRunning = false;
        m_lastActiveLatencyUs = 0.0;
        m_lastCandidateLatencyUs = 0.0;
        m_lastActiveFailureRate = 0.0;
        m_lastCandidateFailureRate = 0.0;
        m_lastActiveSaturationRate = 0.0;
        m_lastCandidateSaturationRate = 0.0;
        ++m_cyclesRun;
        ++m_cyclesRejected;
        setLastCycleResult(QStringLiteral("failed"), fetchError, 0.0, 0.0, 0, false, manual);
        if (reasonOut) {
            *reasonOut = fetchError;
        }
        return false;
    }

    QVector<TrainingExample> replay = fetchReplaySamples(
        getSettingInt(QStringLiteral("onlineRankerReplaySampleLimit"), kDefaultReplaySampleLimit));

    QVector<TrainingExample> combined;
    combined.reserve(fresh.size() + replay.size());
    combined += fresh;
    combined += replay;

    const double negativeSampleRatio = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerNegativeSampleRatio"),
                         kDefaultNegativeSampleRatio),
        0.0,
        10.0);
    const int maxTrainingBatchSize = std::max(
        60, getSettingInt(QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                          kDefaultMaxTrainingBatchSize));
    QVector<TrainingExample> sampledCombined = sampleTrainingBatch(combined,
                                                                   negativeSampleRatio,
                                                                   maxTrainingBatchSize);

    const BatchAttributionStats batchAttribution = collectBatchAttributionStats(sampledCombined);
    m_lastBatchPositiveExamples = batchAttribution.positiveExamples;
    m_lastBatchContextHits = batchAttribution.contextHits;
    m_lastBatchDigestHits = batchAttribution.digestHits;
    m_lastBatchQueryOnlyHits = batchAttribution.queryOnlyHits;
    m_lastBatchUnattributedPositives = batchAttribution.unattributedPositives;
    m_lastBatchAttributedRate = batchAttribution.attributedRate;
    m_lastBatchContextRate = batchAttribution.contextRate;
    m_lastBatchDigestRate = batchAttribution.digestRate;
    m_lastBatchQueryOnlyRate = batchAttribution.queryOnlyRate;
    m_lastBatchUnattributedRate = batchAttribution.unattributedRate;
    m_lastBatchContextDigestRate = batchAttribution.contextDigestRate;

    if (sampledCombined.size() < 60) {
        m_cycleRunning = false;
        m_lastActiveLatencyUs = 0.0;
        m_lastCandidateLatencyUs = 0.0;
        m_lastActiveFailureRate = 0.0;
        m_lastCandidateFailureRate = 0.0;
        m_lastActiveSaturationRate = 0.0;
        m_lastCandidateSaturationRate = 0.0;
        ++m_cyclesRun;
        ++m_cyclesRejected;
        setLastCycleResult(QStringLiteral("rejected"),
                           QStringLiteral("not_enough_training_examples"),
                           0.0,
                           0.0,
                           sampledCombined.size(),
                           false,
                           manual);
        if (reasonOut) {
            *reasonOut = QStringLiteral("not_enough_training_examples");
        }
        return false;
    }

    const int promotionGateMinPositives = std::max(
        1, getSettingInt(QStringLiteral("onlineRankerPromotionGateMinPositives"),
                         kDefaultPromotionGateMinPositives));
    const double promotionMinAttributedRate = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                         kDefaultPromotionMinAttributedRate),
        0.0,
        1.0);
    const double promotionMinContextDigestRate = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                         kDefaultPromotionMinContextDigestRate),
        0.0,
        1.0);

    if (batchAttribution.positiveExamples >= promotionGateMinPositives) {
        QString attributionGateReason;
        if (batchAttribution.attributedRate + 1e-9 < promotionMinAttributedRate) {
            attributionGateReason = QStringLiteral("attribution_quality_gate_failed_attributed_rate");
        } else if (batchAttribution.contextDigestRate + 1e-9 < promotionMinContextDigestRate) {
            attributionGateReason = QStringLiteral("attribution_quality_gate_failed_context_digest_rate");
        }
        if (!attributionGateReason.isEmpty()) {
            m_cycleRunning = false;
            m_lastActiveLatencyUs = 0.0;
            m_lastCandidateLatencyUs = 0.0;
            m_lastActiveFailureRate = 0.0;
            m_lastCandidateFailureRate = 0.0;
            m_lastActiveSaturationRate = 0.0;
            m_lastCandidateSaturationRate = 0.0;
            ++m_cyclesRun;
            ++m_cyclesRejected;
            setLastCycleResult(QStringLiteral("rejected"),
                               attributionGateReason,
                               0.0,
                               0.0,
                               sampledCombined.size(),
                               false,
                               manual);
            if (reasonOut) {
                *reasonOut = attributionGateReason;
            }
            return false;
        }
    }

    OnlineRanker::TrainConfig cfg;
    cfg.epochs = std::max(1, getSettingInt(QStringLiteral("onlineRankerEpochs"), 3));
    cfg.learningRate = std::clamp(getSettingDouble(QStringLiteral("onlineRankerLearningRate"), 0.05), 1e-4, 0.5);
    cfg.l2 = std::clamp(getSettingDouble(QStringLiteral("onlineRankerL2"), 1e-6), 0.0, 0.1);
    cfg.minExamples = std::max(40, getSettingInt(QStringLiteral("onlineRankerMinExamples"), 120));
    cfg.promotionLatencyUsMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                         kDefaultPromotionLatencyUsMax),
        10.0,
        1000000.0);
    cfg.promotionLatencyRegressionPctMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                         kDefaultPromotionLatencyRegressionPctMax),
        0.0,
        1000.0);
    cfg.promotionPredictionFailureRateMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                         kDefaultPromotionPredictionFailureRateMax),
        0.0,
        1.0);
    cfg.promotionSaturationRateMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                         kDefaultPromotionSaturationRateMax),
        0.0,
        1.0);

    OnlineRanker::TrainMetrics activeMetrics;
    OnlineRanker::TrainMetrics candidateMetrics;
    QString rejectReason;
    const bool coreMlBackend = m_coreMlRanker && m_coreMlRanker->hasModel();
    const bool promoted = coreMlBackend
        ? m_coreMlRanker->trainAndPromote(sampledCombined,
                                          cfg,
                                          &activeMetrics,
                                          &candidateMetrics,
                                          &rejectReason)
        : m_ranker->trainAndPromote(sampledCombined,
                                    cfg,
                                    &activeMetrics,
                                    &candidateMetrics,
                                    &rejectReason);

    m_lastActiveLatencyUs = activeMetrics.avgPredictionLatencyUs;
    m_lastCandidateLatencyUs = candidateMetrics.avgPredictionLatencyUs;
    m_lastActiveFailureRate = activeMetrics.predictionFailureRate;
    m_lastCandidateFailureRate = candidateMetrics.predictionFailureRate;
    m_lastActiveSaturationRate = activeMetrics.probabilitySaturationRate;
    m_lastCandidateSaturationRate = candidateMetrics.probabilitySaturationRate;

    if (promoted) {
        for (const TrainingExample& ex : fresh) {
            QString replayError;
            addToReplayReservoir(ex, &replayError);
        }

        if (!consumedIds.isEmpty()) {
            QString idCsv;
            idCsv.reserve(consumedIds.size() * 12);
            for (int i = 0; i < consumedIds.size(); ++i) {
                if (i > 0) {
                    idCsv += QLatin1Char(',');
                }
                idCsv += QString::number(consumedIds.at(i));
            }
            execSql(m_db, QStringLiteral(
                "UPDATE training_examples_v1 SET consumed = 1 WHERE id IN (%1)")
                    .arg(idCsv)
                    .toUtf8()
                    .constData());
        }
    }

    m_cycleRunning = false;
    ++m_cyclesRun;
    if (promoted) {
        ++m_cyclesSucceeded;
        setLastCycleResult(QStringLiteral("succeeded"),
                           QStringLiteral("promoted"),
                           activeMetrics.logLoss,
                           candidateMetrics.logLoss,
                           sampledCombined.size(),
                           true,
                           manual);
        const QString promotedVersion = coreMlBackend
            ? m_coreMlRanker->modelVersion()
            : m_ranker->modelVersion();
        setSetting(QStringLiteral("onlineRankerActiveVersion"), promotedVersion);
        setModelState(QStringLiteral("rollback_version"), previousVersion);
        setModelState(QStringLiteral("active_version"), promotedVersion);
        setModelState(QStringLiteral("active_backend"), coreMlBackend
            ? QStringLiteral("coreml")
            : QStringLiteral("native_sgd"));
    } else {
        ++m_cyclesRejected;
        const QString reason = rejectReason.isEmpty()
            ? QStringLiteral("candidate_not_promoted")
            : rejectReason;
        setLastCycleResult(QStringLiteral("rejected"),
                           reason,
                           activeMetrics.logLoss,
                           candidateMetrics.logLoss,
                           sampledCombined.size(),
                           false,
                           manual);
    }

    if (reasonOut) {
        *reasonOut = promoted ? QStringLiteral("promoted")
                              : (rejectReason.isEmpty()
                                    ? QStringLiteral("candidate_not_promoted")
                                    : rejectReason);
    }
    return promoted;
}

bool LearningEngine::maybeRunIdleCycle(QString* reasonOut)
{
    bool shouldRun = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (reasonOut) {
            reasonOut->clear();
        }

        if (!m_db) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("learning_not_initialized");
            }
            return false;
        }

        const bool learningEnabled = getSettingBool(QStringLiteral("learningEnabled"), false);
        if (!learningEnabled) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("learning_disabled");
            }
            return false;
        }

        const QString rolloutMode = canonicalRolloutMode(
            getSetting(QStringLiteral("onlineRankerRolloutMode"),
                       QString::fromLatin1(kRolloutInstrumentationOnly)),
            nullptr);
        if (!rolloutAllowsTraining(rolloutMode)) {
            ++m_fallbackRolloutMode;
            setSetting(QStringLiteral("onlineRankerFallbackRolloutMode"),
                       QString::number(m_fallbackRolloutMode));
            if (reasonOut) {
                *reasonOut = QStringLiteral("rollout_mode_blocks_training");
            }
            return false;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool pauseOnInput = getSettingBool(QStringLiteral("learningPauseOnUserInput"), true);
        if (pauseOnInput && (nowMs - m_lastUserActivityMs) < kIdleGapMs) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("user_recently_active");
            }
            return false;
        }

        if ((nowMs - m_lastCycleStartedMs) < kMinCycleIntervalMs) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("cooldown_active");
            }
            return false;
        }

        if (m_cycleRunning) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("cycle_in_progress");
            }
            return false;
        }

        QString budgetReason;
        if (!passesResourceBudgetsUnlocked(&budgetReason)) {
            if (reasonOut) {
                *reasonOut = budgetReason;
            }
            ++m_fallbackResourceBudget;
            setSetting(QStringLiteral("onlineRankerFallbackResourceBudget"),
                       QString::number(m_fallbackResourceBudget));
            return false;
        }

        shouldRun = true;
    }

    if (!shouldRun) {
        return false;
    }
    return triggerLearningCycle(/*manual=*/false, reasonOut);
}

double LearningEngine::scoreBoostForResult(const SearchResult& result,
                                           const QueryContext& context,
                                           QueryClass queryClass,
                                           float routerConfidence,
                                           float semanticNeed,
                                           int rank,
                                           int queryTokenCount,
                                           double blendAlpha) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!getSettingBool(QStringLiteral("learningEnabled"), false)) {
        ++m_fallbackLearningDisabled;
        if ((m_fallbackLearningDisabled % 50) == 0) {
            setSetting(QStringLiteral("onlineRankerFallbackLearningDisabled"),
                       QString::number(m_fallbackLearningDisabled));
        }
        return 0.0;
    }

    const QString rolloutMode = canonicalRolloutMode(
        getSetting(QStringLiteral("onlineRankerRolloutMode"),
                   QString::fromLatin1(kRolloutInstrumentationOnly)),
        nullptr);
    if (!rolloutAllowsServing(rolloutMode)) {
        ++m_fallbackRolloutMode;
        if ((m_fallbackRolloutMode % 50) == 0) {
            setSetting(QStringLiteral("onlineRankerFallbackRolloutMode"),
                       QString::number(m_fallbackRolloutMode));
        }
        return 0.0;
    }

    const QVector<double> features = buildFeatureVector(result,
                                                        context,
                                                        queryClass,
                                                        routerConfidence,
                                                        semanticNeed,
                                                        rank,
                                                        queryTokenCount);
    if (m_coreMlRanker && m_coreMlRanker->hasModel()) {
        bool ok = false;
        const double coreMlBoost = m_coreMlRanker->boost(features, blendAlpha, &ok);
        if (ok) {
            return coreMlBoost;
        }
    }

    if (m_ranker && m_ranker->hasModel()) {
        return m_ranker->boost(features, blendAlpha);
    }

    ++m_fallbackMissingModel;
    if ((m_fallbackMissingModel % 50) == 0) {
        setSetting(QStringLiteral("onlineRankerFallbackMissingModel"),
                   QString::number(m_fallbackMissingModel));
    }
    return 0.0;
}

QJsonObject LearningEngine::healthSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool coreMlAvailable = (m_coreMlRanker && m_coreMlRanker->hasModel());
    const bool nativeAvailable = (m_ranker && m_ranker->hasModel());
    const int metricsWindowDays = std::max(
        1, getSettingInt(QStringLiteral("onlineRankerHealthWindowDays"), kDefaultHealthWindowDays));
    const int recentCycleHistoryLimit = std::max(
        1, getSettingInt(QStringLiteral("onlineRankerRecentCycleHistoryLimit"),
                         kDefaultRecentCycleHistoryLimit));
    const int promotionGateMinPositives = std::max(
        1, getSettingInt(QStringLiteral("onlineRankerPromotionGateMinPositives"),
                         kDefaultPromotionGateMinPositives));
    const double promotionMinAttributedRate = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                         kDefaultPromotionMinAttributedRate),
        0.0,
        1.0);
    const double promotionMinContextDigestRate = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                         kDefaultPromotionMinContextDigestRate),
        0.0,
        1.0);
    const double promotionLatencyUsMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                         kDefaultPromotionLatencyUsMax),
        10.0,
        1000000.0);
    const double promotionLatencyRegressionPctMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                         kDefaultPromotionLatencyRegressionPctMax),
        0.0,
        1000.0);
    const double promotionPredictionFailureRateMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                         kDefaultPromotionPredictionFailureRateMax),
        0.0,
        1.0);
    const double promotionSaturationRateMax = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                         kDefaultPromotionSaturationRateMax),
        0.0,
        1.0);
    const double negativeSampleRatio = std::clamp(
        getSettingDouble(QStringLiteral("onlineRankerNegativeSampleRatio"),
                         kDefaultNegativeSampleRatio),
        0.0,
        10.0);
    const int maxTrainingBatchSize = std::max(
        60, getSettingInt(QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                          kDefaultMaxTrainingBatchSize));
    const QString rolloutMode = canonicalRolloutMode(
        getSetting(QStringLiteral("onlineRankerRolloutMode"),
                   QString::fromLatin1(kRolloutInstrumentationOnly)),
        nullptr);

    QJsonObject health;
    health[QStringLiteral("initialized")] = (m_db != nullptr);
    health[QStringLiteral("modelAvailable")] = (coreMlAvailable || nativeAvailable);
    health[QStringLiteral("modelVersion")] = coreMlAvailable
        ? m_coreMlRanker->modelVersion()
        : (m_ranker ? m_ranker->modelVersion() : QString());
    health[QStringLiteral("activeBackend")] = coreMlAvailable
        ? QStringLiteral("coreml")
        : (nativeAvailable ? QStringLiteral("native_sgd") : QStringLiteral("none"));
    health[QStringLiteral("coreMlModelAvailable")] = coreMlAvailable;
    health[QStringLiteral("coreMlUpdatable")] =
        (m_coreMlRanker && m_coreMlRanker->isUpdatable());
    health[QStringLiteral("nativeModelAvailable")] = nativeAvailable;
    health[QStringLiteral("lastCycleStatus")] = m_lastCycleStatus;
    health[QStringLiteral("lastCycleReason")] = m_lastCycleReason;
    health[QStringLiteral("lastCycleAtMs")] = m_lastCycleAtMs;
    health[QStringLiteral("lastActiveLoss")] = m_lastActiveLoss;
    health[QStringLiteral("lastCandidateLoss")] = m_lastCandidateLoss;
    health[QStringLiteral("lastActiveLatencyUs")] = m_lastActiveLatencyUs;
    health[QStringLiteral("lastCandidateLatencyUs")] = m_lastCandidateLatencyUs;
    health[QStringLiteral("lastActivePredictionFailureRate")] = m_lastActiveFailureRate;
    health[QStringLiteral("lastCandidatePredictionFailureRate")] = m_lastCandidateFailureRate;
    health[QStringLiteral("lastActiveSaturationRate")] = m_lastActiveSaturationRate;
    health[QStringLiteral("lastCandidateSaturationRate")] = m_lastCandidateSaturationRate;
    health[QStringLiteral("lastSampleCount")] = m_lastSampleCount;
    health[QStringLiteral("lastPromoted")] = m_lastPromoted;
    health[QStringLiteral("lastManual")] = m_lastManual;
    health[QStringLiteral("cyclesRun")] = m_cyclesRun;
    health[QStringLiteral("cyclesSucceeded")] = m_cyclesSucceeded;
    health[QStringLiteral("cyclesRejected")] = m_cyclesRejected;
    health[QStringLiteral("replaySize")] = replaySize();
    health[QStringLiteral("replayCapacity")] = std::max(256,
        getSettingInt(QStringLiteral("onlineRankerReplayCapacity"), kDefaultReplayCapacity));
    health[QStringLiteral("replaySeenCount")] = static_cast<qint64>(m_replaySeenCount);
    const int pending = pendingExamples();
    health[QStringLiteral("pendingExamples")] = pending;
    health[QStringLiteral("queueDepth")] = pending;
    health[QStringLiteral("fallbackMissingModel")] = m_fallbackMissingModel;
    health[QStringLiteral("fallbackLearningDisabled")] = m_fallbackLearningDisabled;
    health[QStringLiteral("fallbackResourceBudget")] = m_fallbackResourceBudget;
    health[QStringLiteral("fallbackRolloutMode")] = m_fallbackRolloutMode;
    health[QStringLiteral("behaviorStreamEnabled")] =
        getSettingBool(QStringLiteral("behaviorStreamEnabled"), false);
    health[QStringLiteral("learningEnabled")] =
        getSettingBool(QStringLiteral("learningEnabled"), false);
    health[QStringLiteral("onlineRankerRolloutMode")] = rolloutMode;
    health[QStringLiteral("rolloutAllowsTraining")] = rolloutAllowsTraining(rolloutMode);
    health[QStringLiteral("rolloutAllowsServing")] = rolloutAllowsServing(rolloutMode);
    health[QStringLiteral("learningPauseOnUserInput")] =
        getSettingBool(QStringLiteral("learningPauseOnUserInput"), true);
    QJsonObject captureScope;
    captureScope[QStringLiteral("appActivityEnabled")] =
        getSettingBool(QStringLiteral("behaviorCaptureAppActivityEnabled"), true);
    captureScope[QStringLiteral("inputActivityEnabled")] =
        getSettingBool(QStringLiteral("behaviorCaptureInputActivityEnabled"), true);
    captureScope[QStringLiteral("searchEventsEnabled")] =
        getSettingBool(QStringLiteral("behaviorCaptureSearchEventsEnabled"), true);
    captureScope[QStringLiteral("windowTitleHashEnabled")] =
        getSettingBool(QStringLiteral("behaviorCaptureWindowTitleHashEnabled"), true);
    captureScope[QStringLiteral("browserHostHashEnabled")] =
        getSettingBool(QStringLiteral("behaviorCaptureBrowserHostHashEnabled"), true);
    health[QStringLiteral("captureScope")] = captureScope;
    QJsonObject lastBatchAttribution;
    lastBatchAttribution[QStringLiteral("positiveExamples")] = m_lastBatchPositiveExamples;
    lastBatchAttribution[QStringLiteral("contextHits")] = m_lastBatchContextHits;
    lastBatchAttribution[QStringLiteral("digestHits")] = m_lastBatchDigestHits;
    lastBatchAttribution[QStringLiteral("queryOnlyHits")] = m_lastBatchQueryOnlyHits;
    lastBatchAttribution[QStringLiteral("unattributedPositives")] = m_lastBatchUnattributedPositives;
    lastBatchAttribution[QStringLiteral("attributedRate")] = m_lastBatchAttributedRate;
    lastBatchAttribution[QStringLiteral("contextRate")] = m_lastBatchContextRate;
    lastBatchAttribution[QStringLiteral("digestRate")] = m_lastBatchDigestRate;
    lastBatchAttribution[QStringLiteral("queryOnlyRate")] = m_lastBatchQueryOnlyRate;
    lastBatchAttribution[QStringLiteral("unattributedRate")] = m_lastBatchUnattributedRate;
    lastBatchAttribution[QStringLiteral("contextDigestRate")] = m_lastBatchContextDigestRate;
    health[QStringLiteral("lastBatchAttribution")] = lastBatchAttribution;
    QJsonObject promotionAttributionGate;
    promotionAttributionGate[QStringLiteral("minPositiveExamples")] = promotionGateMinPositives;
    promotionAttributionGate[QStringLiteral("minAttributedRate")] = promotionMinAttributedRate;
    promotionAttributionGate[QStringLiteral("minContextDigestRate")] = promotionMinContextDigestRate;
    health[QStringLiteral("promotionAttributionGate")] = promotionAttributionGate;
    QJsonObject promotionRuntimeGate;
    promotionRuntimeGate[QStringLiteral("latencyUsMax")] = promotionLatencyUsMax;
    promotionRuntimeGate[QStringLiteral("latencyRegressionPctMax")] = promotionLatencyRegressionPctMax;
    promotionRuntimeGate[QStringLiteral("predictionFailureRateMax")] = promotionPredictionFailureRateMax;
    promotionRuntimeGate[QStringLiteral("saturationRateMax")] = promotionSaturationRateMax;
    health[QStringLiteral("promotionRuntimeGate")] = promotionRuntimeGate;
    health[QStringLiteral("negativeSampleRatio")] = negativeSampleRatio;
    health[QStringLiteral("maxTrainingBatchSize")] = maxTrainingBatchSize;
    const QJsonArray storedCycleHistory = parseJsonArrayOrEmpty(
        getSetting(QStringLiteral("onlineRankerRecentCycleHistory"), QStringLiteral("[]")));
    QJsonArray recentLearningCycles;
    for (const QJsonValue& value : storedCycleHistory) {
        if (!value.isObject()) {
            continue;
        }
        recentLearningCycles.append(value);
        if (recentLearningCycles.size() >= recentCycleHistoryLimit) {
            break;
        }
    }
    health[QStringLiteral("recentLearningCycles")] = recentLearningCycles;
    health[QStringLiteral("recentLearningCyclesCount")] = recentLearningCycles.size();
    health[QStringLiteral("recentLearningCyclesLimit")] = recentCycleHistoryLimit;
    health[QStringLiteral("metricsWindowDays")] = metricsWindowDays;
    health[QStringLiteral("attributionMetrics")] =
        collectAttributionMetrics(m_db, metricsWindowDays);
    health[QStringLiteral("behaviorCoverageMetrics")] =
        collectBehaviorCoverageMetrics(m_db, metricsWindowDays);
    health[QStringLiteral("cycleRunning")] = m_cycleRunning;
    health[QStringLiteral("lastUserActivityMs")] = m_lastUserActivityMs;
    return health;
}

bool LearningEngine::modelAvailable() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_coreMlRanker && m_coreMlRanker->hasModel())
        || (m_ranker && m_ranker->hasModel());
}

QString LearningEngine::modelVersion() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_coreMlRanker && m_coreMlRanker->hasModel()) {
        return m_coreMlRanker->modelVersion();
    }
    return m_ranker ? m_ranker->modelVersion() : QString();
}

} // namespace bs
