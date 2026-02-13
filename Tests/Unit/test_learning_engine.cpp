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
    void testRecordBehaviorEventSecureAndPrivateFilters();
    void testRecordBehaviorEventCaptureScopeFilter();
    void testBehaviorEventPrunesExpiredRowsOnWrite();
    void testExposureAndPositiveAttribution();
    void testPositiveAttributionPrefersContextEvent();
    void testHealthSnapshotReportsAttributionAndCoverage();
    void testTriggerLearningCycleRejectsAttributionGate();
    void testTriggerLearningCycleAppliesNegativeSampling();
    void testNegativeSamplingTruncatesAtBatchCap();
    void testReplayReservoirCapacityAndSlotsBounded();
    void testRepeatedIdleStyleCyclesKeepBoundedState();
    void testEndToEndExposureAttributionTrainPromote();
    void testScoreBoostFallsBackWhenModelsMissingOrCorrupt();
    void testTriggerLearningCyclePromotesModel();
    void testTriggerLearningCycleRejectsCandidateNotBetter();
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

void TestLearningEngine::testRecordBehaviorEventSecureAndPrivateFilters()
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

    bs::BehaviorEvent secureEvent;
    secureEvent.eventId = QStringLiteral("evt-secure-1");
    secureEvent.source = QStringLiteral("system_collector");
    secureEvent.eventType = QStringLiteral("input_activity");
    secureEvent.privacyFlags.secureInput = true;
    secureEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(secureEvent));

    bs::BehaviorEvent privateEvent;
    privateEvent.eventId = QStringLiteral("evt-private-1");
    privateEvent.source = QStringLiteral("system_collector");
    privateEvent.eventType = QStringLiteral("input_activity");
    privateEvent.privacyFlags.privateContext = true;
    privateEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(privateEvent));

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

void TestLearningEngine::testBehaviorEventPrunesExpiredRowsOnWrite()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    upsertSetting(store.rawDb(),
                  QStringLiteral("behaviorRawRetentionDays"),
                  QStringLiteral("1"));

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    const double nowSec = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const double staleSec = nowSec - (3.0 * 24.0 * 60.0 * 60.0);

    sqlite3_stmt* insertStmt = nullptr;
    const char* insertSql = R"(
        INSERT INTO behavior_events_v1 (
            event_id,
            timestamp,
            source,
            event_type,
            app_bundle_id,
            input_meta,
            mouse_meta,
            privacy_flags,
            attribution_confidence,
            created_at
        ) VALUES ('evt-stale', ?1, 'system_collector', 'input_activity', 'com.example.old',
                  '{}', '{}', '{}', 0.5, ?2)
    )";
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(), insertSql, -1, &insertStmt, nullptr), SQLITE_OK);
    sqlite3_bind_double(insertStmt, 1, staleSec);
    sqlite3_bind_double(insertStmt, 2, staleSec);
    QCOMPARE(sqlite3_step(insertStmt), SQLITE_DONE);
    sqlite3_finalize(insertStmt);

    sqlite3_stmt* countStmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &countStmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(countStmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(countStmt, 0), 1);
    sqlite3_finalize(countStmt);

    bs::BehaviorEvent freshEvent;
    freshEvent.eventId = QStringLiteral("evt-fresh");
    freshEvent.source = QStringLiteral("system_collector");
    freshEvent.eventType = QStringLiteral("app_activated");
    freshEvent.appBundleId = QStringLiteral("com.example.new");
    freshEvent.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(freshEvent));

    sqlite3_stmt* verifyStmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT event_id FROM behavior_events_v1 ORDER BY event_id ASC",
                                -1,
                                &verifyStmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(verifyStmt), SQLITE_ROW);
    const char* onlyEvent = reinterpret_cast<const char*>(sqlite3_column_text(verifyStmt, 0));
    QCOMPARE(QString::fromUtf8(onlyEvent ? onlyEvent : ""), QStringLiteral("evt-fresh"));
    QCOMPARE(sqlite3_step(verifyStmt), SQLITE_DONE);
    sqlite3_finalize(verifyStmt);
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

void TestLearningEngine::testNegativeSamplingTruncatesAtBatchCap()
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
                  QStringLiteral("3.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                  QStringLiteral("100"));

    // 150 positives + 150 negatives with batch cap 100 => sampled batch truncates to 100.
    for (int i = 0; i < 300; ++i) {
        const int label = i < 150 ? 1 : 0;
        const double f0 = label > 0 ? 0.80 : 0.20;
        const double f1 = label > 0 ? 0.70 : 0.30;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("sample-cap-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          f0,
                          f1,
                          label > 0 ? 1.0 : 0.0);
    }

    QString reason;
    engine.triggerLearningCycle(true, &reason);

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("lastSampleCount")).toInt(), 100);
    QCOMPARE(health.value(QStringLiteral("maxTrainingBatchSize")).toInt(), 100);

    const QJsonObject lastBatch = health.value(QStringLiteral("lastBatchAttribution")).toObject();
    QCOMPARE(lastBatch.value(QStringLiteral("positiveExamples")).toInt(), 100);
    QCOMPARE(lastBatch.value(QStringLiteral("contextHits")).toInt(), 100);
    QCOMPARE(lastBatch.value(QStringLiteral("digestHits")).toInt(), 0);
    QCOMPARE(lastBatch.value(QStringLiteral("queryOnlyHits")).toInt(), 0);
}

void TestLearningEngine::testReplayReservoirCapacityAndSlotsBounded()
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
                  QStringLiteral("onlineRankerReplayCapacity"),
                  QStringLiteral("256"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerNegativeSampleRatio"),
                  QStringLiteral("3.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                  QStringLiteral("1200"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                  QStringLiteral("1000000"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                  QStringLiteral("1000"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                  QStringLiteral("1.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                  QStringLiteral("1.0"));

    // 600 rows so replay insert path exceeds capacity and executes replacement/drop logic.
    for (int i = 0; i < 600; ++i) {
        const int label = (i % 3 == 0) ? 1 : 0;
        const double f0 = label > 0 ? 0.75 : 0.25;
        const double f1 = label > 0 ? 0.65 : 0.35;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("sample-replay-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          f0,
                          f1,
                          label > 0 ? 1.0 : 0.0);
    }

    QString reason;
    const bool promoted = engine.triggerLearningCycle(true, &reason);
    QVERIFY2(promoted, qPrintable(reason));

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("replayCapacity")).toInt(), 256);
    QCOMPARE(health.value(QStringLiteral("replaySize")).toInt(), 256);
    QVERIFY(health.value(QStringLiteral("replaySeenCount")).toInteger() >= 600);
    QVERIFY(health.value(QStringLiteral("replaySeenCount")).toInteger()
            > health.value(QStringLiteral("replaySize")).toInteger());

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*), COUNT(DISTINCT slot), "
                                "MIN(slot), MAX(slot) "
                                "FROM replay_reservoir_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 256);
    QCOMPARE(sqlite3_column_int(stmt, 1), 256);
    QCOMPARE(sqlite3_column_int(stmt, 2), 0);
    QCOMPARE(sqlite3_column_int(stmt, 3), 255);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testRepeatedIdleStyleCyclesKeepBoundedState()
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
    QVERIFY(engine.setConsent(true, true, false, {}));

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
                  QStringLiteral("160"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerReplayCapacity"),
                  QStringLiteral("64"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerRecentCycleHistoryLimit"),
                  QStringLiteral("5"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionGateMinPositives"),
                  QStringLiteral("1"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                  QStringLiteral("0.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                  QStringLiteral("0.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                  QStringLiteral("1000000"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                  QStringLiteral("1000"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                  QStringLiteral("1.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                  QStringLiteral("1.0"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("learningIdleCpuPctMax"),
                  QStringLiteral("1000"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("learningMemMbMax"),
                  QStringLiteral("4096"));
    upsertSetting(store.rawDb(),
                  QStringLiteral("learningThermalMax"),
                  QStringLiteral("10"));

    int sampleOrdinal = 0;
    for (int cycle = 0; cycle < 14; ++cycle) {
        // Keep injecting fresh labeled rows so repeated non-manual cycles exercise bounded state.
        for (int i = 0; i < 96; ++i) {
            const int label = ((i + cycle) % 2 == 0) ? 1 : 0;
            const double f0 = label > 0 ? 0.78 : 0.22;
            const double f1 = label > 0 ? 0.68 : 0.32;
            insertTrainingRow(store.rawDb(),
                              QStringLiteral("sample-loop-%1").arg(sampleOrdinal++),
                              itemIdOpt.value(),
                              label,
                              f0,
                              f1,
                              label > 0 ? 1.0 : 0.0);
        }

        QString reason;
        engine.triggerLearningCycle(false, &reason);
        QVERIFY(!reason.trimmed().isEmpty());
    }

    const QJsonObject health = engine.healthSnapshot();
    QVERIFY(health.value(QStringLiteral("cyclesRun")).toInt(0) >= 10);
    QCOMPARE(health.value(QStringLiteral("replayCapacity")).toInt(), 64);
    QVERIFY(health.value(QStringLiteral("replaySize")).toInt(0) <= 64);
    QCOMPARE(health.value(QStringLiteral("recentLearningCyclesLimit")).toInt(), 5);

    const QJsonArray recent = health.value(QStringLiteral("recentLearningCycles")).toArray();
    QCOMPARE(recent.size(), 5);
    for (int i = 1; i < recent.size(); ++i) {
        const qint64 prevIndex =
            recent.at(i - 1).toObject().value(QStringLiteral("cycleIndex")).toInteger(0);
        const qint64 currentIndex =
            recent.at(i).toObject().value(QStringLiteral("cycleIndex")).toInteger(0);
        QVERIFY(prevIndex >= currentIndex);
    }

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*), COUNT(DISTINCT slot), "
                                "COALESCE(MIN(slot), 0), COALESCE(MAX(slot), 0) "
                                "FROM replay_reservoir_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    const int replayCount = sqlite3_column_int(stmt, 0);
    const int distinctSlots = sqlite3_column_int(stmt, 1);
    const int minSlot = sqlite3_column_int(stmt, 2);
    const int maxSlot = sqlite3_column_int(stmt, 3);
    QVERIFY(replayCount <= 64);
    QCOMPARE(distinctSlots, replayCount);
    if (replayCount > 0) {
        QVERIFY(minSlot >= 0);
        QVERIFY(maxSlot < 64);
    }
    sqlite3_finalize(stmt);
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

void TestLearningEngine::testTriggerLearningCycleRejectsCandidateNotBetter()
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
                  QStringLiteral("40"));

    // Balanced labels with near-constant features produce little to no incremental gain
    // after the first promotion, so a subsequent candidate should be rejected.
    for (int i = 0; i < 120; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("not-better-seed-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          0.0,
                          0.0);
    }

    QString firstReason;
    const bool firstPromoted = engine.triggerLearningCycle(true, &firstReason);
    QVERIFY2(firstPromoted, qPrintable(firstReason));
    const QString firstVersion = engine.modelVersion();
    QVERIFY(!firstVersion.isEmpty());

    for (int i = 0; i < 120; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("not-better-second-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          0.0,
                          0.0);
    }

    QString secondReason;
    const bool secondPromoted = engine.triggerLearningCycle(true, &secondReason);
    QVERIFY(!secondPromoted);
    QCOMPARE(secondReason, QStringLiteral("candidate_not_better_than_active"));

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("lastCycleStatus")).toString(),
             QStringLiteral("rejected"));
    QCOMPARE(health.value(QStringLiteral("lastCycleReason")).toString(),
             QStringLiteral("candidate_not_better_than_active"));
    QCOMPARE(health.value(QStringLiteral("modelVersion")).toString(), firstVersion);
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
