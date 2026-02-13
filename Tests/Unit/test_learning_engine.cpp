#include <QtTest/QtTest>

#include "core/index/sqlite_store.h"
#include "core/learning/learning_engine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <sqlite3.h>

namespace {

std::optional<int64_t> seedItem(bs::SQLiteStore& store,
                                const QString& path,
                                const QString& name)
{
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    return store.upsertItem(path,
                            name,
                            QFileInfo(path).suffix(),
                            bs::ItemKind::Markdown,
                            1024,
                            now,
                            now,
                            QString(),
                            QStringLiteral("normal"),
                            QFileInfo(path).absolutePath());
}

void insertTrainingRow(sqlite3* db,
                       const QString& sampleId,
                       int64_t itemId,
                       int label,
                       double f0,
                       double f1,
                       double attributionConfidence = 1.0)
{
    const QString featuresJson = QStringLiteral("[%1,%2,0,0,0,0,0,0,1,0,1,1,0]")
        .arg(f0, 0, 'f', 6)
        .arg(f1, 0, 'f', 6);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
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
            attribution_confidence,
            consumed
        ) VALUES (?1, ?2, 'report', 'report', ?3, '/tmp/report.md', ?4, 1.0, ?5, ?6, 0)
    )";

    QCOMPARE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    const QByteArray sampleUtf8 = sampleId.toUtf8();
    const QByteArray featuresUtf8 = featuresJson.toUtf8();
    sqlite3_bind_text(stmt, 1, sampleUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    sqlite3_bind_int64(stmt, 3, itemId);
    sqlite3_bind_int(stmt, 4, label);
    sqlite3_bind_text(stmt, 5, featuresUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 6, std::clamp(attributionConfidence, 0.0, 1.0));
    QCOMPARE(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

void upsertSetting(sqlite3* db, const QString& key, const QString& value)
{
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        INSERT INTO settings (key, value) VALUES (?1, ?2)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";
    QCOMPARE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, valueUtf8.constData(), -1, SQLITE_TRANSIENT);
    QCOMPARE(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

} // namespace

class TestLearningEngine : public QObject {
    Q_OBJECT

private slots:
    void testRecordBehaviorEventWithConsent();
    void testRecordBehaviorEventDenylistFilter();
    void testRecordBehaviorEventRedactedFilter();
    void testRecordBehaviorEventCaptureScopeFilter();
    void testExposureAndPositiveAttribution();
    void testPositiveAttributionPrefersContextEvent();
    void testHealthSnapshotReportsAttributionAndCoverage();
    void testTriggerLearningCycleRejectsAttributionGate();
    void testTriggerLearningCycleAppliesNegativeSampling();
    void testEndToEndExposureAttributionTrainPromote();
    void testScoreBoostFallsBackWhenModelsMissingOrCorrupt();
    void testTriggerLearningCyclePromotesModel();
    void testCoreMlBootstrapSeededFromEnvOverride();
};

void TestLearningEngine::testRecordBehaviorEventWithConsent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    auto itemIdOpt = seedItem(store,
                              QDir(tempDir.path()).filePath(QStringLiteral("notes.md")),
                              QStringLiteral("notes.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::BehaviorEvent event;
    event.eventId = QStringLiteral("evt-1");
    event.source = QStringLiteral("betterspotlight");
    event.eventType = QStringLiteral("query_submitted");
    event.itemId = itemIdOpt.value();
    event.itemPath = QDir(tempDir.path()).filePath(QStringLiteral("notes.md"));
    event.timestamp = QDateTime::currentDateTimeUtc();

    QVERIFY(engine.recordBehaviorEvent(event));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testRecordBehaviorEventDenylistFilter()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {QStringLiteral("com.example.secret")}));

    bs::BehaviorEvent event;
    event.eventId = QStringLiteral("evt-deny-1");
    event.source = QStringLiteral("betterspotlight");
    event.eventType = QStringLiteral("query_submitted");
    event.appBundleId = QStringLiteral("com.example.secret");
    event.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(event));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testRecordBehaviorEventRedactedFilter()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::BehaviorEvent event;
    event.eventId = QStringLiteral("evt-redacted-1");
    event.source = QStringLiteral("system_collector");
    event.eventType = QStringLiteral("input_activity");
    event.privacyFlags.redacted = true;
    event.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(event));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testRecordBehaviorEventCaptureScopeFilter()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true,
                              true,
                              true,
                              {},
                              nullptr,
                              QString(),
                              false,
                              false,
                              false,
                              false,
                              false));

    bs::BehaviorEvent appEvent;
    appEvent.eventId = QStringLiteral("evt-capture-app");
    appEvent.source = QStringLiteral("system_collector");
    appEvent.eventType = QStringLiteral("app_activated");
    appEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(appEvent));

    bs::BehaviorEvent inputEvent;
    inputEvent.eventId = QStringLiteral("evt-capture-input");
    inputEvent.source = QStringLiteral("system_collector");
    inputEvent.eventType = QStringLiteral("input_activity");
    inputEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(inputEvent));

    bs::BehaviorEvent searchEvent;
    searchEvent.eventId = QStringLiteral("evt-capture-search");
    searchEvent.source = QStringLiteral("betterspotlight");
    searchEvent.eventType = QStringLiteral("query_submitted");
    searchEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(searchEvent));

    bs::BehaviorEvent keptEvent;
    keptEvent.eventId = QStringLiteral("evt-capture-custom");
    keptEvent.source = QStringLiteral("betterspotlight");
    keptEvent.eventType = QStringLiteral("custom_activity");
    keptEvent.windowTitleHash = QStringLiteral("hash-window");
    keptEvent.browserHostHash = QStringLiteral("hash-host");
    keptEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(keptEvent));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COALESCE(window_title_hash, ''), "
                                "COALESCE(browser_host_hash, '') "
                                "FROM behavior_events_v1 WHERE event_id = 'evt-capture-custom'",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const char* rawWindowHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* rawBrowserHostHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    QCOMPARE(QString::fromUtf8(rawWindowHash ? rawWindowHash : ""), QString());
    QCOMPARE(QString::fromUtf8(rawBrowserHostHash ? rawBrowserHostHash : ""), QString());
    sqlite3_finalize(stmt);

    const QJsonObject health = engine.healthSnapshot();
    const QJsonObject scope = health.value(QStringLiteral("captureScope")).toObject();
    QVERIFY(scope.contains(QStringLiteral("appActivityEnabled")));
    QVERIFY(scope.contains(QStringLiteral("inputActivityEnabled")));
    QVERIFY(scope.contains(QStringLiteral("searchEventsEnabled")));
    QVERIFY(scope.contains(QStringLiteral("windowTitleHashEnabled")));
    QVERIFY(scope.contains(QStringLiteral("browserHostHashEnabled")));
    QVERIFY(!scope.value(QStringLiteral("appActivityEnabled")).toBool(true));
    QVERIFY(!scope.value(QStringLiteral("inputActivityEnabled")).toBool(true));
    QVERIFY(!scope.value(QStringLiteral("searchEventsEnabled")).toBool(true));
    QVERIFY(!scope.value(QStringLiteral("windowTitleHashEnabled")).toBool(true));
    QVERIFY(!scope.value(QStringLiteral("browserHostHashEnabled")).toBool(true));
}

void TestLearningEngine::testExposureAndPositiveAttribution()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString path = QDir(tempDir.path()).filePath(QStringLiteral("report.md"));
    auto itemIdOpt = seedItem(store, path, QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::SearchResult result;
    result.itemId = itemIdOpt.value();
    result.path = path;
    result.name = QStringLiteral("report.md");
    result.score = 120.0;
    result.semanticNormalized = 0.64;
    result.crossEncoderScore = 0.55f;

    bs::QueryContext context;
    context.contextEventId = QStringLiteral("ctx-1");
    context.activityDigest = QStringLiteral("digest-1");

    QVERIFY(engine.recordExposure(QStringLiteral("report"),
                                  result,
                                  context,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  0));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM training_examples_v1 WHERE label IS NULL",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    QVERIFY(engine.recordPositiveInteraction(QStringLiteral("report"),
                                             itemIdOpt.value(),
                                             path,
                                             QStringLiteral("com.apple.finder"),
                                             QStringLiteral("ctx-1"),
                                             QStringLiteral("digest-1"),
                                             QDateTime::currentDateTimeUtc()));

    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM training_examples_v1 WHERE label = 1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testPositiveAttributionPrefersContextEvent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString path = QDir(tempDir.path()).filePath(QStringLiteral("report.md"));
    auto itemIdOpt = seedItem(store, path, QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::SearchResult result;
    result.itemId = itemIdOpt.value();
    result.path = path;
    result.name = QStringLiteral("report.md");
    result.score = 120.0;
    result.semanticNormalized = 0.64;
    result.crossEncoderScore = 0.55f;

    bs::QueryContext contextA;
    contextA.contextEventId = QStringLiteral("ctx-a");
    contextA.activityDigest = QStringLiteral("digest-a");
    QVERIFY(engine.recordExposure(QStringLiteral("report"),
                                  result,
                                  contextA,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  0));

    bs::QueryContext contextB;
    contextB.contextEventId = QStringLiteral("ctx-b");
    contextB.activityDigest = QStringLiteral("digest-b");
    QVERIFY(engine.recordExposure(QStringLiteral("report"),
                                  result,
                                  contextB,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  1));

    QVERIFY(engine.recordPositiveInteraction(QStringLiteral("report"),
                                             itemIdOpt.value(),
                                             path,
                                             QStringLiteral("com.apple.finder"),
                                             QStringLiteral("ctx-b"),
                                             QStringLiteral("digest-b"),
                                             QDateTime::currentDateTimeUtc()));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM training_examples_v1 "
                                "WHERE label = 1 AND context_event_id = 'ctx-b'",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM training_examples_v1 "
                                "WHERE label = 1 AND context_event_id = 'ctx-a'",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testHealthSnapshotReportsAttributionAndCoverage()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString path = QDir(tempDir.path()).filePath(QStringLiteral("report.md"));
    auto itemIdOpt = seedItem(store, path, QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::SearchResult result;
    result.itemId = itemIdOpt.value();
    result.path = path;
    result.name = QStringLiteral("report.md");
    result.score = 120.0;
    result.semanticNormalized = 0.64;
    result.crossEncoderScore = 0.55f;

    const QDateTime now = QDateTime::currentDateTimeUtc();

    bs::QueryContext contextMatch;
    contextMatch.contextEventId = QStringLiteral("ctx-metrics");
    contextMatch.activityDigest = QStringLiteral("digest-metrics-context");
    QVERIFY(engine.recordExposure(QStringLiteral("query context"),
                                  result,
                                  contextMatch,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  0));
    QVERIFY(engine.recordPositiveInteraction(QStringLiteral("query context"),
                                             itemIdOpt.value(),
                                             path,
                                             QStringLiteral("com.apple.finder"),
                                             QStringLiteral("ctx-metrics"),
                                             QStringLiteral("digest-metrics-context"),
                                             now));

    bs::QueryContext digestMatch;
    digestMatch.activityDigest = QStringLiteral("digest-metrics-only");
    QVERIFY(engine.recordExposure(QStringLiteral("query digest"),
                                  result,
                                  digestMatch,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  1));
    QVERIFY(engine.recordPositiveInteraction(QStringLiteral("query digest"),
                                             itemIdOpt.value(),
                                             path,
                                             QStringLiteral("com.apple.finder"),
                                             QString(),
                                             QStringLiteral("digest-metrics-only"),
                                             now.addSecs(1)));

    bs::QueryContext queryOnlyMatch;
    QVERIFY(engine.recordExposure(QStringLiteral("query only"),
                                  result,
                                  queryOnlyMatch,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  2));
    QVERIFY(engine.recordPositiveInteraction(QStringLiteral("query only"),
                                             itemIdOpt.value(),
                                             path,
                                             QStringLiteral("com.apple.finder"),
                                             QString(),
                                             QString(),
                                             now.addSecs(2)));

    bs::BehaviorEvent eventA;
    eventA.eventId = QStringLiteral("metrics-event-a");
    eventA.source = QStringLiteral("system");
    eventA.eventType = QStringLiteral("activity");
    eventA.appBundleId = QStringLiteral("com.apple.finder");
    eventA.contextEventId = QStringLiteral("ctx-stream-a");
    eventA.activityDigest = QStringLiteral("digest-stream-a");
    eventA.timestamp = now;
    QVERIFY(engine.recordBehaviorEvent(eventA));

    bs::BehaviorEvent eventB;
    eventB.eventId = QStringLiteral("metrics-event-b");
    eventB.source = QStringLiteral("system");
    eventB.eventType = QStringLiteral("activity");
    eventB.appBundleId = QStringLiteral("com.apple.finder");
    eventB.timestamp = now.addSecs(1);
    QVERIFY(engine.recordBehaviorEvent(eventB));

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("metricsWindowDays")).toInt(), 7);
    QVERIFY(health.value(QStringLiteral("recentLearningCycles")).isArray());
    QCOMPARE(health.value(QStringLiteral("recentLearningCyclesCount")).toInt(), 0);

    const QJsonObject attribution = health.value(QStringLiteral("attributionMetrics")).toObject();
    QCOMPARE(attribution.value(QStringLiteral("positiveExamples")).toInt(), 3);
    QCOMPARE(attribution.value(QStringLiteral("attributedExamples")).toInt(), 3);
    QCOMPARE(attribution.value(QStringLiteral("contextHits")).toInt(), 1);
    QCOMPARE(attribution.value(QStringLiteral("digestHits")).toInt(), 1);
    QCOMPARE(attribution.value(QStringLiteral("queryOnlyHits")).toInt(), 1);
    QCOMPARE(attribution.value(QStringLiteral("unattributedPositives")).toInt(), 0);
    QVERIFY(qAbs(attribution.value(QStringLiteral("contextHitRate")).toDouble() - (1.0 / 3.0)) < 0.001);
    QVERIFY(qAbs(attribution.value(QStringLiteral("digestHitRate")).toDouble() - (1.0 / 3.0)) < 0.001);
    QVERIFY(qAbs(attribution.value(QStringLiteral("queryOnlyRate")).toDouble() - (1.0 / 3.0)) < 0.001);
    QVERIFY(qAbs(attribution.value(QStringLiteral("attributedRate")).toDouble() - 1.0) < 0.001);

    const QJsonObject coverage = health.value(QStringLiteral("behaviorCoverageMetrics")).toObject();
    QCOMPARE(coverage.value(QStringLiteral("events")).toInt(), 2);
    QCOMPARE(coverage.value(QStringLiteral("appBundlePresent")).toInt(), 2);
    QCOMPARE(coverage.value(QStringLiteral("activityDigestPresent")).toInt(), 1);
    QCOMPARE(coverage.value(QStringLiteral("contextEventPresent")).toInt(), 1);
    QCOMPARE(coverage.value(QStringLiteral("eventsWithAnyContextSignal")).toInt(), 2);
    QCOMPARE(coverage.value(QStringLiteral("eventsWithFullContextSignals")).toInt(), 1);
    QVERIFY(qAbs(coverage.value(QStringLiteral("activityDigestCoverage")).toDouble() - 0.5) < 0.001);
    QVERIFY(qAbs(coverage.value(QStringLiteral("contextEventCoverage")).toDouble() - 0.5) < 0.001);
    QVERIFY(qAbs(coverage.value(QStringLiteral("anyContextSignalCoverage")).toDouble() - 1.0) < 0.001);
    QVERIFY(qAbs(coverage.value(QStringLiteral("fullContextSignalsCoverage")).toDouble() - 0.5) < 0.001);
}

void TestLearningEngine::testTriggerLearningCycleRejectsAttributionGate()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    auto itemIdOpt = seedItem(store,
                              QDir(tempDir.path()).filePath(QStringLiteral("report.md")),
                              QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerRolloutMode"),
                  QStringLiteral("blended_ranking"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionGateMinPositives"),
                  QStringLiteral("80"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                  QStringLiteral("0.5"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                  QStringLiteral("0.3"));

    for (int i = 0; i < 180; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        const double f0 = label > 0 ? 0.85 : 0.15;
        const double f1 = label > 0 ? 0.75 : 0.25;
        // 0.7 maps to query-only attribution, so context+digest coverage should be 0.
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("gate-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          f0,
                          f1,
                          0.7);
    }

    QString reason;
    const bool promoted = engine.triggerLearningCycle(true, &reason);
    QVERIFY(!promoted);
    QCOMPARE(reason, QStringLiteral("attribution_quality_gate_failed_context_digest_rate"));

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("lastCycleStatus")).toString(),
             QStringLiteral("rejected"));
    QCOMPARE(health.value(QStringLiteral("lastCycleReason")).toString(),
             QStringLiteral("attribution_quality_gate_failed_context_digest_rate"));

    const QJsonObject lastBatch = health.value(QStringLiteral("lastBatchAttribution")).toObject();
    QCOMPARE(lastBatch.value(QStringLiteral("positiveExamples")).toInt(), 90);
    QCOMPARE(lastBatch.value(QStringLiteral("contextHits")).toInt(), 0);
    QCOMPARE(lastBatch.value(QStringLiteral("digestHits")).toInt(), 0);
    QCOMPARE(lastBatch.value(QStringLiteral("queryOnlyHits")).toInt(), 90);
    const QJsonArray recentCycles = health.value(QStringLiteral("recentLearningCycles")).toArray();
    QVERIFY(!recentCycles.isEmpty());
    const QJsonObject latestCycle = recentCycles.first().toObject();
    QCOMPARE(latestCycle.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    QCOMPARE(latestCycle.value(QStringLiteral("reason")).toString(),
             QStringLiteral("attribution_quality_gate_failed_context_digest_rate"));
    QVERIFY(latestCycle.value(QStringLiteral("batchAttribution")).isObject());
    QVERIFY(health.value(QStringLiteral("recentLearningCyclesCount")).toInt() >= 1);

    const QJsonObject gate = health.value(QStringLiteral("promotionAttributionGate")).toObject();
    QCOMPARE(gate.value(QStringLiteral("minPositiveExamples")).toInt(), 80);
    QVERIFY(qAbs(gate.value(QStringLiteral("minAttributedRate")).toDouble() - 0.5) < 0.0001);
    QVERIFY(qAbs(gate.value(QStringLiteral("minContextDigestRate")).toDouble() - 0.3) < 0.0001);
}

void TestLearningEngine::testTriggerLearningCycleAppliesNegativeSampling()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    auto itemIdOpt = seedItem(store,
                              QDir(tempDir.path()).filePath(QStringLiteral("report.md")),
                              QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerRolloutMode"),
                  QStringLiteral("blended_ranking"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerMinExamples"),
                  QStringLiteral("20"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerNegativeSampleRatio"),
                  QStringLiteral("1.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                  QStringLiteral("1200"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                  QStringLiteral("2222"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                  QStringLiteral("12"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                  QStringLiteral("0.07"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                  QStringLiteral("0.98"));

    // 30 positives + 120 negatives.
    for (int i = 0; i < 150; ++i) {
        const int label = i < 30 ? 1 : 0;
        const double f0 = label > 0 ? 0.85 : 0.15;
        const double f1 = label > 0 ? 0.75 : 0.25;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("sample-neg-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          f0,
                          f1,
                          label > 0 ? 1.0 : 0.0);
    }

    QString reason;
    engine.triggerLearningCycle(true, &reason);

    const QJsonObject health = engine.healthSnapshot();
    // Ratio=1.0 keeps all positives and samples negatives up to positive count.
    QCOMPARE(health.value(QStringLiteral("lastSampleCount")).toInt(), 60);
    QVERIFY(qAbs(health.value(QStringLiteral("negativeSampleRatio")).toDouble() - 1.0) < 0.0001);
    QCOMPARE(health.value(QStringLiteral("maxTrainingBatchSize")).toInt(), 1200);
    QVERIFY(health.value(QStringLiteral("promotionRuntimeGate")).isObject());
    const QJsonObject runtimeGate = health.value(QStringLiteral("promotionRuntimeGate")).toObject();
    QVERIFY(qAbs(runtimeGate.value(QStringLiteral("latencyUsMax")).toDouble() - 2222.0) < 0.0001);
    QVERIFY(qAbs(runtimeGate.value(QStringLiteral("latencyRegressionPctMax")).toDouble() - 12.0) < 0.0001);
    QVERIFY(qAbs(runtimeGate.value(QStringLiteral("predictionFailureRateMax")).toDouble() - 0.07) < 0.0001);
    QVERIFY(qAbs(runtimeGate.value(QStringLiteral("saturationRateMax")).toDouble() - 0.98) < 0.0001);
    QVERIFY(health.contains(QStringLiteral("lastCandidateLatencyUs")));
    QVERIFY(health.contains(QStringLiteral("lastCandidatePredictionFailureRate")));
    QVERIFY(health.contains(QStringLiteral("lastCandidateSaturationRate")));
}

void TestLearningEngine::testEndToEndExposureAttributionTrainPromote()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString positivePath = QDir(tempDir.path()).filePath(QStringLiteral("report.md"));
    const QString negativePath = QDir(tempDir.path()).filePath(QStringLiteral("notes.md"));
    auto positiveItemIdOpt = seedItem(store, positivePath, QStringLiteral("report.md"));
    auto negativeItemIdOpt = seedItem(store, negativePath, QStringLiteral("notes.md"));
    QVERIFY(positiveItemIdOpt.has_value());
    QVERIFY(negativeItemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerRolloutMode"),
                  QStringLiteral("blended_ranking"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerMinExamples"),
                  QStringLiteral("40"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerNegativeStaleSeconds"),
                  QStringLiteral("1"));

    bs::SearchResult positiveResult;
    positiveResult.itemId = positiveItemIdOpt.value();
    positiveResult.path = positivePath;
    positiveResult.name = QStringLiteral("report.md");
    positiveResult.score = 175.0;
    positiveResult.semanticNormalized = 0.92;
    positiveResult.crossEncoderScore = 0.86f;

    bs::SearchResult negativeResult;
    negativeResult.itemId = negativeItemIdOpt.value();
    negativeResult.path = negativePath;
    negativeResult.name = QStringLiteral("notes.md");
    negativeResult.score = 38.0;
    negativeResult.semanticNormalized = 0.12;
    negativeResult.crossEncoderScore = 0.08f;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < 70; ++i) {
        const QString contextEventId = QStringLiteral("ctx-e2e-%1").arg(i);
        const QString activityDigest = QStringLiteral("digest-e2e-%1").arg(i);

        bs::QueryContext context;
        context.contextEventId = contextEventId;
        context.activityDigest = activityDigest;
        context.frontmostAppBundleId = QStringLiteral("com.apple.finder");

        QVERIFY(engine.recordExposure(QStringLiteral("report"),
                                      positiveResult,
                                      context,
                                      bs::QueryClass::NaturalLanguage,
                                      0.92f,
                                      0.85f,
                                      0));
        QVERIFY(engine.recordExposure(QStringLiteral("report"),
                                      negativeResult,
                                      context,
                                      bs::QueryClass::NaturalLanguage,
                                      0.92f,
                                      0.85f,
                                      1));
        QVERIFY(engine.recordPositiveInteraction(QStringLiteral("report"),
                                                 positiveItemIdOpt.value(),
                                                 positivePath,
                                                 QStringLiteral("com.apple.finder"),
                                                 contextEventId,
                                                 activityDigest,
                                                 now.addSecs(i)));
    }

    // Let unlabeled exposures age into sampled negatives.
    QTest::qWait(2200);

    QString reason;
    const bool promoted = engine.triggerLearningCycle(true, &reason);
    QVERIFY2(promoted, qPrintable(reason));

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("lastCycleStatus")).toString(),
             QStringLiteral("succeeded"));
    QCOMPARE(health.value(QStringLiteral("lastCycleReason")).toString(),
             QStringLiteral("promoted"));
    QVERIFY(health.value(QStringLiteral("lastSampleCount")).toInt() >= 70);
    QVERIFY(health.value(QStringLiteral("replaySize")).toInt() > 0);

    const QJsonObject attribution = health.value(QStringLiteral("attributionMetrics")).toObject();
    QVERIFY(attribution.value(QStringLiteral("positiveExamples")).toInt() >= 70);
    QVERIFY(attribution.value(QStringLiteral("contextHitRate")).toDouble() > 0.95);

    const QJsonArray cycles = health.value(QStringLiteral("recentLearningCycles")).toArray();
    QVERIFY(!cycles.isEmpty());
    const QJsonObject latest = cycles.first().toObject();
    QCOMPARE(latest.value(QStringLiteral("status")).toString(),
             QStringLiteral("succeeded"));
    QCOMPARE(latest.value(QStringLiteral("reason")).toString(),
             QStringLiteral("promoted"));
}

void TestLearningEngine::testScoreBoostFallsBackWhenModelsMissingOrCorrupt()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString itemPath = QDir(tempDir.path()).filePath(QStringLiteral("report.md"));
    auto itemIdOpt = seedItem(store, itemPath, QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    const QString runtimeDataDir = QDir(tempDir.path()).filePath(QStringLiteral("runtime-data"));
    const QString invalidBootstrapModelDir = QDir(runtimeDataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(invalidBootstrapModelDir));
    QFile invalidBootstrapPayload(QDir(invalidBootstrapModelDir).filePath(QStringLiteral("dummy.bin")));
    QVERIFY(invalidBootstrapPayload.open(QIODevice::WriteOnly | QIODevice::Truncate));
    invalidBootstrapPayload.write("invalid-coreml-bootstrap");
    invalidBootstrapPayload.close();

    const QString invalidNativeWeightsPath = QDir(runtimeDataDir).filePath(
        QStringLiteral("models/online-ranker-v1/active/weights.json"));
    QVERIFY(QDir().mkpath(QFileInfo(invalidNativeWeightsPath).absolutePath()));
    QFile invalidNativeWeights(invalidNativeWeightsPath);
    QVERIFY(invalidNativeWeights.open(QIODevice::WriteOnly | QIODevice::Truncate));
    invalidNativeWeights.write("{invalid-json");
    invalidNativeWeights.close();

    bs::LearningEngine engine(store.rawDb(), runtimeDataDir);
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerRolloutMode"),
                  QStringLiteral("blended_ranking"));

    bs::SearchResult result;
    result.itemId = itemIdOpt.value();
    result.path = itemPath;
    result.name = QStringLiteral("report.md");
    result.score = 75.0;
    result.semanticNormalized = 0.4;
    result.crossEncoderScore = 0.3f;

    bs::QueryContext context;
    context.frontmostAppBundleId = QStringLiteral("com.apple.finder");

    const double boost = engine.scoreBoostForResult(result,
                                                    context,
                                                    bs::QueryClass::NaturalLanguage,
                                                    0.7f,
                                                    0.5f,
                                                    0,
                                                    1,
                                                    0.2);
    QCOMPARE(boost, 0.0);

    const QJsonObject health = engine.healthSnapshot();
    QVERIFY(!health.value(QStringLiteral("modelAvailable")).toBool(true));
    QVERIFY(!health.value(QStringLiteral("coreMlModelAvailable")).toBool(true));
    QVERIFY(!health.value(QStringLiteral("nativeModelAvailable")).toBool(true));
    QCOMPARE(health.value(QStringLiteral("fallbackMissingModel")).toInt(), 1);
}

void TestLearningEngine::testTriggerLearningCyclePromotesModel()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    auto itemIdOpt = seedItem(store,
                              QDir(tempDir.path()).filePath(QStringLiteral("report.md")),
                              QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerRolloutMode"),
                  QStringLiteral("blended_ranking"));

    for (int i = 0; i < 180; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        const double f0 = label > 0 ? 0.85 : 0.15;
        const double f1 = label > 0 ? 0.75 : 0.25;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("seed-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          f0,
                          f1);
    }

    QString reason;
    const bool promoted = engine.triggerLearningCycle(true, &reason);
    QVERIFY2(promoted, qPrintable(reason));
    QVERIFY(engine.modelAvailable());

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("lastCycleStatus")).toString(),
             QStringLiteral("succeeded"));
}

void TestLearningEngine::testCoreMlBootstrapSeededFromEnvOverride()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString sourceBootstrapDir =
        QDir(tempDir.path()).filePath(QStringLiteral("bootstrap-source"));
    const QString sourceModelDir = QDir(sourceBootstrapDir).filePath(
        QStringLiteral("online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(sourceModelDir));

    QFile dummyModelFile(QDir(sourceModelDir).filePath(QStringLiteral("dummy.bin")));
    QVERIFY(dummyModelFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    dummyModelFile.write("seed");
    dummyModelFile.close();

    QFile metadataFile(QDir(sourceBootstrapDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    metadataFile.write("{\"version\":\"seed\"}");
    metadataFile.close();

    const QString runtimeDataDir = QDir(tempDir.path()).filePath(QStringLiteral("runtime-data"));
    const QByteArray originalEnv = qgetenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR");
    qputenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR", sourceBootstrapDir.toUtf8());

    bs::LearningEngine engine(store.rawDb(), runtimeDataDir);
    const bool initialized = engine.initialize();

    if (originalEnv.isEmpty()) {
        qunsetenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR");
    } else {
        qputenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR", originalEnv);
    }
    QVERIFY(initialized);

    const QString seededModelDir = QDir(runtimeDataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    const QString seededMetadataPath = QDir(runtimeDataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/metadata.json"));

    QVERIFY(QFileInfo::exists(QDir(seededModelDir).filePath(QStringLiteral("dummy.bin"))));
    QVERIFY(QFileInfo::exists(seededMetadataPath));
}

QTEST_MAIN(TestLearningEngine)
#include "test_learning_engine.moc"
