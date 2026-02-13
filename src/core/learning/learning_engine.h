#pragma once

#include "core/learning/behavior_types.h"
#include "core/shared/search_result.h"
#include "core/query/structured_query.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QSet>

#include <memory>
#include <mutex>

struct sqlite3;

namespace bs {

class OnlineRanker;
class CoreMlRanker;

class LearningEngine {
public:
    LearningEngine(sqlite3* db, QString dataDir);
    ~LearningEngine();

    bool initialize();

    void noteUserActivity();

    bool recordBehaviorEvent(const BehaviorEvent& event, QString* errorOut = nullptr);
    bool setConsent(bool behaviorStreamEnabled,
                    bool learningEnabled,
                    bool learningPauseOnUserInput,
                    const QStringList& denylistApps,
                    QString* errorOut = nullptr,
                    const QString& rolloutMode = QString(),
                    bool captureAppActivityEnabled = true,
                    bool captureInputActivityEnabled = true,
                    bool captureSearchEventsEnabled = true,
                    bool captureWindowTitleHashEnabled = true,
                    bool captureBrowserHostHashEnabled = true);

    bool recordExposure(const QString& query,
                        const SearchResult& result,
                        const QueryContext& context,
                        QueryClass queryClass,
                        float routerConfidence,
                        float semanticNeed,
                        int rank,
                        QString* errorOut = nullptr);

    bool recordPositiveInteraction(const QString& query,
                                   int64_t itemId,
                                   const QString& path,
                                   const QString& appBundleId,
                                   const QString& contextEventId,
                                   const QString& activityDigest,
                                   const QDateTime& timestamp,
                                   QString* errorOut = nullptr);

    double scoreBoostForResult(const SearchResult& result,
                               const QueryContext& context,
                               QueryClass queryClass,
                               float routerConfidence,
                               float semanticNeed,
                               int rank,
                               int queryTokenCount,
                               double blendAlpha) const;

    bool maybeRunIdleCycle(QString* reasonOut = nullptr);
    bool triggerLearningCycle(bool manual, QString* reasonOut = nullptr);

    QJsonObject healthSnapshot() const;
    bool modelAvailable() const;
    QString modelVersion() const;

private:
    bool setSetting(const QString& key, const QString& value) const;
    QString getSetting(const QString& key, const QString& fallback = QString()) const;
    bool getSettingBool(const QString& key, bool fallback) const;
    int getSettingInt(const QString& key, int fallback) const;
    double getSettingDouble(const QString& key, double fallback) const;
    bool setModelState(const QString& key, const QString& value) const;
    QSet<QString> readDenylistApps() const;
    void maybePruneExpiredDataUnlocked();
    bool passesResourceBudgetsUnlocked(QString* reasonOut) const;

    QVector<double> buildFeatureVector(const SearchResult& result,
                                       const QueryContext& context,
                                       QueryClass queryClass,
                                       float routerConfidence,
                                       float semanticNeed,
                                       int rank,
                                       int queryTokenCount) const;

    static QString featuresToJson(const QVector<double>& features);
    static QVector<double> featuresFromJson(const QString& encoded);

    bool addToReplayReservoir(const TrainingExample& example, QString* errorOut = nullptr);
    QVector<TrainingExample> fetchReplaySamples(int limit) const;
    QVector<TrainingExample> fetchFreshExamplesForTraining(int limit, int staleNegativeSeconds,
                                                           QVector<int64_t>* consumedIdsOut,
                                                           QString* errorOut);
    int replaySize() const;
    int pendingExamples() const;

    void setLastCycleResult(const QString& status,
                            const QString& reason,
                            double activeLoss,
                            double candidateLoss,
                            int sampleCount,
                            bool promoted,
                            bool manual);

    sqlite3* m_db = nullptr;
    QString m_dataDir;
    QString m_modelRootDir;
    QString m_modelPath;
    std::unique_ptr<OnlineRanker> m_ranker;
    std::unique_ptr<CoreMlRanker> m_coreMlRanker;

    mutable std::mutex m_mutex;
    mutable QJsonObject m_cachedHealth;

    uint64_t m_replaySeenCount = 0;
    qint64 m_lastUserActivityMs = 0;
    qint64 m_lastCycleStartedMs = 0;
    bool m_cycleRunning = false;

    QString m_lastCycleStatus = QStringLiteral("never_run");
    QString m_lastCycleReason;
    double m_lastActiveLoss = 0.0;
    double m_lastCandidateLoss = 0.0;
    int m_lastSampleCount = 0;
    bool m_lastPromoted = false;
    bool m_lastManual = false;
    qint64 m_lastCycleAtMs = 0;
    int m_cyclesRun = 0;
    int m_cyclesSucceeded = 0;
    int m_cyclesRejected = 0;
    qint64 m_lastPruneAtMs = 0;
    int m_lastBatchPositiveExamples = 0;
    int m_lastBatchContextHits = 0;
    int m_lastBatchDigestHits = 0;
    int m_lastBatchQueryOnlyHits = 0;
    int m_lastBatchUnattributedPositives = 0;
    double m_lastBatchAttributedRate = 0.0;
    double m_lastBatchContextRate = 0.0;
    double m_lastBatchDigestRate = 0.0;
    double m_lastBatchQueryOnlyRate = 0.0;
    double m_lastBatchUnattributedRate = 0.0;
    double m_lastBatchContextDigestRate = 0.0;
    mutable int m_fallbackMissingModel = 0;
    mutable int m_fallbackLearningDisabled = 0;
    mutable int m_fallbackResourceBudget = 0;
    mutable int m_fallbackRolloutMode = 0;
};

} // namespace bs
