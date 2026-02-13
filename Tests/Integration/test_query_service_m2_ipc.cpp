#include <QtTest/QtTest>

#include "core/index/sqlite_store.h"
#include "core/shared/chunk.h"
#include "core/shared/ipc_messages.h"
#include "ipc_test_utils.h"
#include "service_process_harness.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <sqlite3.h>

#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace {

std::optional<qint64> seedItemWithChunks(bs::SQLiteStore& store,
                                         const QString& rootDir,
                                         const QString& fileName,
                                         const QStringList& chunkTexts)
{
    const QString path = QDir(rootDir).filePath(fileName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return std::nullopt;
    }
    file.write(chunkTexts.join(QLatin1Char('\n')).toUtf8());
    file.close();

    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    auto itemIdOpt = store.upsertItem(path,
                                      fileName,
                                      QFileInfo(path).suffix(),
                                      bs::ItemKind::Markdown,
                                      QFileInfo(path).size(),
                                      now,
                                      now,
                                      QString(),
                                      QStringLiteral("normal"),
                                      rootDir);
    if (!itemIdOpt.has_value()) {
        return std::nullopt;
    }

    std::vector<bs::Chunk> chunks;
    chunks.reserve(static_cast<size_t>(chunkTexts.size()));
    int byteOffset = 0;
    for (int i = 0; i < chunkTexts.size(); ++i) {
        const QString text = chunkTexts.at(i).trimmed();
        if (text.isEmpty()) {
            continue;
        }
        bs::Chunk chunk;
        chunk.chunkId = bs::computeChunkId(path, i);
        chunk.filePath = path;
        chunk.chunkIndex = i;
        chunk.content = text;
        chunk.byteOffset = byteOffset;
        byteOffset += text.toUtf8().size() + 1;
        chunks.push_back(std::move(chunk));
    }
    if (chunks.empty()) {
        return std::nullopt;
    }
    if (!store.insertChunks(itemIdOpt.value(), fileName, path, chunks)) {
        return std::nullopt;
    }
    return static_cast<qint64>(itemIdOpt.value());
}

std::optional<QString> readLearningModelStateValue(const QString& dbPath, const QString& key)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.toUtf8().constData(),
                        &db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return std::nullopt;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM learning_model_state_v1 WHERE key = ?1 LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }

    const QByteArray keyUtf8 = key.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_TRANSIENT);

    std::optional<QString> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* rawValue = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result = rawValue ? QString::fromUtf8(rawValue) : QString();
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

QString denseFeaturesJson(double value, int dim = 13)
{
    QJsonArray features;
    for (int i = 0; i < dim; ++i) {
        features.append(value);
    }
    return QString::fromUtf8(QJsonDocument(features).toJson(QJsonDocument::Compact));
}

bool insertTrainingExample(sqlite3* db,
                           const QString& sampleId,
                           qint64 itemId,
                           const QString& path,
                           int label,
                           const QString& featuresJson,
                           double createdAtSec)
{
    if (!db) {
        return false;
    }

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
        ) VALUES (?1, ?2, 'report', 'report', ?3, ?4, ?5, 1.0, ?6, 1.0, 0)
    )";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const QByteArray sampleUtf8 = sampleId.toUtf8();
    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray featuresUtf8 = featuresJson.toUtf8();
    sqlite3_bind_text(stmt, 1, sampleUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, createdAtSec);
    sqlite3_bind_int64(stmt, 3, itemId);
    sqlite3_bind_text(stmt, 4, pathUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, label);
    sqlite3_bind_text(stmt, 6, featuresUtf8.constData(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

} // namespace

class TestQueryServiceM2Ipc : public QObject {
    Q_OBJECT

private slots:
    void testQueryM2IpcContract();
    void testLearningNativePromotionPersistsModelState();
    void testLearningServingFallbackWithoutModels();
    void testLearningIdleCycleReasonGates();
    void testLearningPromotionRejectsOnRuntimeGate();
    void testLearningSchedulerReasonCounts();
    void testLearningSchedulerTransitionSequence();
    void testSetLearningConsentRejectsInvalidRolloutMode();
    void testLearningCycleHistoryBoundedViaRpc();
    void testRecordBehaviorEventPrivacyExclusionsBlockAttribution();
    void testRecordBehaviorEventDuplicateIdIgnored();
    void testDuplicateReplayDoesNotInflateQueuesOrScheduler();
    void testOnlineRankerServingRespectsRolloutModes();
};

void TestQueryServiceM2Ipc::testQueryM2IpcContract()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for fixture seeding");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("report.md"),
        {
            QStringLiteral("quarterly report summary for vector rebuild"),
            QStringLiteral("pipeline test content for deterministic embeddings"),
            QStringLiteral("final chunk for mapping persistence checks"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed primary report item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("report.md"));

    auto secondItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("ops-notes.md"),
        {
            QStringLiteral("operations notes chunk alpha"),
            QStringLiteral("operations notes chunk beta"),
        });
    QVERIFY2(secondItemIdOpt.has_value(), "Failed to seed secondary report item");

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 8000;
    QVERIFY2(harness.start(launch), "Failed to start query service");

    {
        QJsonObject params;
        params[QStringLiteral("selectedItemId")] = 1;
        const QJsonObject response = harness.request(QStringLiteral("record_interaction"), params);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("hello");
        const QJsonObject response = harness.request(QStringLiteral("record_interaction"), params);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("bad-id");
        params[QStringLiteral("selectedItemId")] = -4;
        const QJsonObject response = harness.request(QStringLiteral("record_interaction"), params);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("report");
        params[QStringLiteral("selectedItemId")] = seededItemId;
        params[QStringLiteral("selectedPath")] = seededPath;
        params[QStringLiteral("matchType")] = QStringLiteral("exact");
        params[QStringLiteral("resultPosition")] = 1;
        params[QStringLiteral("frontmostApp")] = QStringLiteral("Finder");
        params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
        params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-m2-ipc-report");
        params[QStringLiteral("activityDigest")] = QStringLiteral("digest-m2-ipc-report");
        const QJsonObject response =
            harness.request(QStringLiteral("record_interaction"), params, 10000);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("recorded")).toBool(false));
    }

    {
        QJsonObject params;
        params[QStringLiteral("limit")] = -5;
        const QJsonObject response = harness.request(QStringLiteral("get_path_preferences"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("directories")).isArray());
    }
    {
        QJsonObject params;
        params[QStringLiteral("limit")] = 999;
        const QJsonObject response = harness.request(QStringLiteral("get_path_preferences"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonArray directories =
            bs::test::resultPayload(response).value(QStringLiteral("directories")).toArray();
        QVERIFY(directories.size() <= 200);
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_file_type_affinity"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.contains(QStringLiteral("codeOpens")));
        QVERIFY(result.contains(QStringLiteral("documentOpens")));
        QVERIFY(result.contains(QStringLiteral("primaryAffinity")));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("run_aggregation"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.contains(QStringLiteral("aggregated")));
        QVERIFY(result.contains(QStringLiteral("cleanedUp")));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("export_interaction_data"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        const QJsonArray interactions = result.value(QStringLiteral("interactions")).toArray();
        QVERIFY(result.value(QStringLiteral("interactions")).isArray());
        QVERIFY(result.value(QStringLiteral("count")).toInt() >= 1);

        bool foundSeeded = false;
        for (const QJsonValue& value : interactions) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("query")).toString() == QLatin1String("report")
                && row.value(QStringLiteral("itemId")).toInteger(0) == seededItemId) {
                foundSeeded = true;
                QCOMPARE(row.value(QStringLiteral("frontmostApp")).toString(),
                         QStringLiteral("com.apple.finder"));
                break;
            }
        }
        QVERIFY(foundSeeded);
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("learning")).isObject());
        const QJsonObject learning = result.value(QStringLiteral("learning")).toObject();
        QVERIFY(learning.value(QStringLiteral("attributionMetrics")).isObject());
        QVERIFY(learning.value(QStringLiteral("behaviorCoverageMetrics")).isObject());
        QVERIFY(learning.value(QStringLiteral("promotionAttributionGate")).isObject());
        QVERIFY(learning.value(QStringLiteral("promotionRuntimeGate")).isObject());
        QVERIFY(learning.value(QStringLiteral("recentLearningCycles")).isArray());
        QVERIFY(learning.value(QStringLiteral("scheduler")).isObject());
        QVERIFY(learning.value(QStringLiteral("captureScope")).isObject());
    }

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = true;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        QJsonArray denylist;
        denylist.append(QStringLiteral("com.example.secret"));
        params[QStringLiteral("denylistApps")] = denylist;
        QJsonObject captureScope;
        captureScope[QStringLiteral("appActivityEnabled")] = false;
        captureScope[QStringLiteral("inputActivityEnabled")] = false;
        captureScope[QStringLiteral("searchEventsEnabled")] = true;
        captureScope[QStringLiteral("windowTitleHashEnabled")] = false;
        captureScope[QStringLiteral("browserHostHashEnabled")] = false;
        params[QStringLiteral("captureScope")] = captureScope;

        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("updated")).toBool(false));
        QVERIFY(result.value(QStringLiteral("learning")).isObject());
        const QJsonObject learning = result.value(QStringLiteral("learning")).toObject();
        QVERIFY(learning.value(QStringLiteral("captureScope")).isObject());
        const QJsonObject returnedScope = learning.value(QStringLiteral("captureScope")).toObject();
        QVERIFY(!returnedScope.value(QStringLiteral("appActivityEnabled")).toBool(true));
        QVERIFY(!returnedScope.value(QStringLiteral("inputActivityEnabled")).toBool(true));
        QVERIFY(returnedScope.value(QStringLiteral("searchEventsEnabled")).toBool(false));
        QVERIFY(!returnedScope.value(QStringLiteral("windowTitleHashEnabled")).toBool(true));
        QVERIFY(!returnedScope.value(QStringLiteral("browserHostHashEnabled")).toBool(true));
        QVERIFY(learning.value(QStringLiteral("scheduler")).isObject());
    }

    {
        QJsonObject params;
        params[QStringLiteral("eventId")] = QStringLiteral("fixture-behavior-1");
        params[QStringLiteral("eventType")] = QStringLiteral("result_open");
        params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
        params[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        params[QStringLiteral("itemId")] = seededItemId;
        params[QStringLiteral("itemPath")] = seededPath;
        params[QStringLiteral("query")] = QStringLiteral("report");
        params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
        params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-behavior-stream-fixture");
        params[QStringLiteral("activityDigest")] = QStringLiteral("digest-behavior-stream-fixture");
        params[QStringLiteral("attributionConfidence")] = 0.95;

        QJsonObject inputMeta;
        inputMeta[QStringLiteral("keyEventCount")] = 4;
        inputMeta[QStringLiteral("shortcutCount")] = 0;
        inputMeta[QStringLiteral("scrollCount")] = 0;
        inputMeta[QStringLiteral("metadataOnly")] = true;
        params[QStringLiteral("inputMeta")] = inputMeta;

        QJsonObject privacyFlags;
        privacyFlags[QStringLiteral("secureInput")] = false;
        privacyFlags[QStringLiteral("privateContext")] = false;
        privacyFlags[QStringLiteral("denylistedApp")] = false;
        privacyFlags[QStringLiteral("redacted")] = false;
        params[QStringLiteral("privacyFlags")] = privacyFlags;

        const QJsonObject response = harness.request(QStringLiteral("record_behavior_event"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("recorded")).toBool(false));
        QVERIFY(result.value(QStringLiteral("attributedPositive")).toBool(false));
        QVERIFY(result.contains(QStringLiteral("idleCycleTriggered")));
        QVERIFY(result.contains(QStringLiteral("idleCycleReason")));
        QVERIFY(result.value(QStringLiteral("learningHealth")).isObject());
        const QJsonObject learning = result.value(QStringLiteral("learningHealth")).toObject();
        QVERIFY(learning.value(QStringLiteral("attributionMetrics")).isObject());
        QVERIFY(learning.value(QStringLiteral("behaviorCoverageMetrics")).isObject());
        QVERIFY(learning.value(QStringLiteral("promotionAttributionGate")).isObject());
        QVERIFY(learning.value(QStringLiteral("promotionRuntimeGate")).isObject());
        QVERIFY(learning.value(QStringLiteral("recentLearningCycles")).isArray());
        const QJsonObject attribution =
            learning.value(QStringLiteral("attributionMetrics")).toObject();
        const QJsonObject coverage =
            learning.value(QStringLiteral("behaviorCoverageMetrics")).toObject();
        QVERIFY(attribution.value(QStringLiteral("positiveExamples")).toInt(0) >= 1);
        QVERIFY(coverage.value(QStringLiteral("events")).toInt(0) >= 1);
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("report");
        params[QStringLiteral("limit")] = 5;
        params[QStringLiteral("debug")] = true;

        const QJsonObject response = harness.request(QStringLiteral("search"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("results")).isArray());
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(!debugInfo.isEmpty());
        QVERIFY(debugInfo.value(QStringLiteral("contextFallbackApplied")).toBool(false));
        QCOMPARE(debugInfo.value(QStringLiteral("contextFrontmostAppBundleId")).toString(),
                 QStringLiteral("com.apple.finder"));
        QCOMPARE(debugInfo.value(QStringLiteral("contextEventId")).toString(),
                 QStringLiteral("ctx-behavior-stream-fixture"));
        QCOMPARE(debugInfo.value(QStringLiteral("contextActivityDigest")).toString(),
                 QStringLiteral("digest-behavior-stream-fixture"));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("trigger_learning_cycle"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.contains(QStringLiteral("promoted")));
        QVERIFY(result.contains(QStringLiteral("reason")));
        QVERIFY(result.value(QStringLiteral("learning")).isObject());
        const QJsonObject learning = result.value(QStringLiteral("learning")).toObject();
        QVERIFY(!learning.isEmpty());
        QVERIFY(learning.value(QStringLiteral("lastCycleStatus")).isString());
        QVERIFY(learning.value(QStringLiteral("lastCycleStatus")).toString()
                != QLatin1String("never_run"));
        QVERIFY(learning.value(QStringLiteral("lastSampleCount")).toInt(-1) >= 0);
        const QJsonArray recentCycles =
            learning.value(QStringLiteral("recentLearningCycles")).toArray();
        QVERIFY(!recentCycles.isEmpty());
        const QJsonObject latestCycle = recentCycles.first().toObject();
        QCOMPARE(latestCycle.value(QStringLiteral("reason")).toString(),
                 result.value(QStringLiteral("reason")).toString());
        QCOMPARE(latestCycle.value(QStringLiteral("promoted")).toBool(false),
                 result.value(QStringLiteral("promoted")).toBool(false));
    }

    bs::test::ServiceProcessHarness inferenceHarness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig inferenceLaunch;
    inferenceLaunch.homeDir = tempHome.path();
    inferenceLaunch.dataDir = dataDir;
    inferenceLaunch.env.insert(
        QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    inferenceLaunch.env.insert(
        QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    inferenceLaunch.startTimeoutMs = 15000;
    inferenceLaunch.connectTimeoutMs = 15000;
    inferenceLaunch.readyTimeoutMs = 30000;
    inferenceLaunch.requestDefaultTimeoutMs = 7000;
    QVERIFY2(inferenceHarness.start(inferenceLaunch),
             "Failed to start inference service for rebuild-offload coverage");
    {
        bool inferenceReady = false;
        QElapsedTimer waitTimer;
        waitTimer.start();
        while (waitTimer.elapsed() < 5000) {
            const QJsonObject healthResponse =
                inferenceHarness.request(QStringLiteral("get_inference_health"), {}, 1500);
            if (bs::test::isResponse(healthResponse)) {
                inferenceReady = true;
                break;
            }
            QTest::qWait(100);
        }
        QVERIFY2(inferenceReady, "Inference service did not become ready before rebuild");
    }

    QJsonObject rebuildParams;
    QJsonArray includePaths;
    includePaths.append(docsDir.path() + QStringLiteral("/"));
    includePaths.append(QString());
    includePaths.append(docsDir.path());
    includePaths.append(QDir(docsDir.path()).filePath(QStringLiteral("sub/..")));
    rebuildParams[QStringLiteral("includePaths")] = includePaths;
    rebuildParams[QStringLiteral("targetGeneration")] = QStringLiteral("  v9  ");

    const QJsonObject rebuildResponse =
        harness.request(QStringLiteral("rebuild_vector_index"), rebuildParams, 15000);
    QVERIFY2(bs::test::isResponse(rebuildResponse), "rebuild_vector_index should start in test mode");
    const QJsonObject rebuildResult = bs::test::resultPayload(rebuildResponse);
    QVERIFY(rebuildResult.value(QStringLiteral("started")).toBool(false));
    QVERIFY(!rebuildResult.value(QStringLiteral("alreadyRunning")).toBool(true));
    QVERIFY(rebuildResult.value(QStringLiteral("runId")).toInteger(0) > 0);
    QCOMPARE(rebuildResult.value(QStringLiteral("targetGeneration")).toString(),
             QStringLiteral("v9"));

    const QJsonObject secondResponse =
        harness.request(QStringLiteral("rebuildVectorIndex"), rebuildParams, 5000);
    QVERIFY(bs::test::isResponse(secondResponse));
    const QJsonObject secondResult = bs::test::resultPayload(secondResponse);
    QVERIFY(secondResult.value(QStringLiteral("alreadyRunning")).toBool(false));

    const QString targetGeneration =
        rebuildResult.value(QStringLiteral("targetGeneration")).toString(QStringLiteral("v2"));
    const QString expectedIndexPath =
        QDir(dataDir).filePath(QStringLiteral("vectors-%1.hnsw").arg(targetGeneration));
    const QString expectedMetaPath =
        QDir(dataDir).filePath(QStringLiteral("vectors-%1.meta").arg(targetGeneration));

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 12000
           && (!QFileInfo::exists(expectedIndexPath) || !QFileInfo::exists(expectedMetaPath))) {
        QTest::qWait(100);
    }
    QVERIFY2(QFileInfo::exists(expectedIndexPath), "Rebuild should persist vector index file");
    QVERIFY2(QFileInfo::exists(expectedMetaPath), "Rebuild should persist vector metadata file");

    const QJsonObject healthResponse = harness.request(QStringLiteral("getHealth"), {}, 5000);
    QVERIFY(bs::test::isResponse(healthResponse));
    const QJsonObject indexHealth = bs::test::resultPayload(healthResponse)
                                        .value(QStringLiteral("indexHealth"))
                                        .toObject();
    const QString finalStatus = indexHealth.value(QStringLiteral("vectorRebuildStatus")).toString();
    QVERIFY(finalStatus == QStringLiteral("running") || finalStatus == QStringLiteral("succeeded"));
    QVERIFY(indexHealth.value(QStringLiteral("vectorRebuildProcessed")).toInt() >= 2);
    QVERIFY(indexHealth.value(QStringLiteral("vectorRebuildEmbedded")).toInt() >= 2);

    QTemporaryDir tempHomeNoFake;
    QTemporaryDir fakeModelsDir;
    QVERIFY(tempHomeNoFake.isValid());
    QVERIFY(fakeModelsDir.isValid());

    const QString dataDirNoFake =
        QDir(tempHomeNoFake.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDirNoFake));

    const QString manifestPath = QDir(fakeModelsDir.path()).filePath(QStringLiteral("manifest.json"));
    {
        QFile manifest(manifestPath);
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QJsonObject models{
            {QStringLiteral("bi-encoder"),
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("broken-model")},
                 {QStringLiteral("file"), QStringLiteral("missing.onnx")},
                 {QStringLiteral("dimensions"), 384},
                 {QStringLiteral("generationId"), QStringLiteral("v2")},
             }},
        };
        const QJsonObject root{{QStringLiteral("models"), models}};
        manifest.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        manifest.close();
    }

    bs::test::ServiceProcessHarness noFakeHarness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig noFakeLaunch;
    noFakeLaunch.homeDir = tempHomeNoFake.path();
    noFakeLaunch.dataDir = dataDirNoFake;
    noFakeLaunch.env.insert(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"), fakeModelsDir.path());
    noFakeLaunch.startTimeoutMs = 15000;
    noFakeLaunch.connectTimeoutMs = 15000;
    noFakeLaunch.readyTimeoutMs = 30000;
    noFakeLaunch.requestDefaultTimeoutMs = 8000;
    QVERIFY2(noFakeHarness.start(noFakeLaunch), "Failed to start no-fake query service");

    QJsonObject noFakeParams;
    QJsonArray noFakePaths;
    noFakePaths.append(docsDir.path());
    noFakeParams[QStringLiteral("includePaths")] = noFakePaths;
    const QJsonObject unsupportedResponse =
        noFakeHarness.request(QStringLiteral("rebuild_vector_index"), noFakeParams, 8000);
    QVERIFY(bs::test::isError(unsupportedResponse));
    QCOMPARE(bs::test::errorPayload(unsupportedResponse).value(QStringLiteral("code")).toInt(),
             static_cast<int>(bs::IpcErrorCode::Unsupported));
}

void TestQueryServiceM2Ipc::testLearningNativePromotionPersistsModelState()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for learning fixture seeding");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    std::vector<std::pair<qint64, QString>> seededItems;
    seededItems.reserve(24);
    for (int i = 0; i < 24; ++i) {
        const QString fileName = QStringLiteral("report-%1.md")
                                     .arg(i + 1, 2, 10, QLatin1Char('0'));
        auto itemIdOpt = seedItemWithChunks(
            fixtureStore,
            docsDir.path(),
            fileName,
            {
                QStringLiteral("report learning fixture line %1").arg(i),
                QStringLiteral("native promotion deterministic coverage %1").arg(i),
            });
        QVERIFY2(itemIdOpt.has_value(), "Failed to seed report item for learning fixture");
        seededItems.emplace_back(itemIdOpt.value(), QDir(docsDir.path()).filePath(fileName));
    }
    QVERIFY(seededItems.size() >= 20);

    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerNegativeStaleSeconds"),
                                    QStringLiteral("1")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                                    QStringLiteral("1000000")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                                    QStringLiteral("1000")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                                    QStringLiteral("1.0")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                                    QStringLiteral("1.0")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerEpochs"), QStringLiteral("2")));

    const QString invalidCoreMlBootstrapDir = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(invalidCoreMlBootstrapDir));
    {
        QFile invalidMarker(QDir(invalidCoreMlBootstrapDir).filePath(QStringLiteral("invalid.bin")));
        QVERIFY(invalidMarker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalidMarker.write("invalid-coreml-bootstrap");
        invalidMarker.close();
    }

    QString modelVersionBefore;
    QString modelVersionAfter;

    {
        bs::test::ServiceProcessHarness harness(
            QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
        bs::test::ServiceLaunchConfig launch;
        launch.homeDir = tempHome.path();
        launch.dataDir = dataDir;
        launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
        launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
        launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
        launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
        launch.startTimeoutMs = 15000;
        launch.connectTimeoutMs = 15000;
        launch.readyTimeoutMs = 30000;
        launch.requestDefaultTimeoutMs = 12000;
        QVERIFY2(harness.start(launch), "Failed to start query service for native learning test");

        {
            QJsonObject params;
            params[QStringLiteral("behaviorStreamEnabled")] = true;
            params[QStringLiteral("learningEnabled")] = true;
            params[QStringLiteral("learningPauseOnUserInput")] = false;
            params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
            const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
            QVERIFY(bs::test::isResponse(response));
            QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("updated")).toBool(false));
        }

        {
            const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
            QVERIFY(bs::test::isResponse(response));
            const QJsonObject learning = bs::test::resultPayload(response)
                                             .value(QStringLiteral("learning"))
                                             .toObject();
            QVERIFY(!learning.isEmpty());
            QVERIFY(!learning.value(QStringLiteral("coreMlModelAvailable")).toBool(true));
            modelVersionBefore = learning.value(QStringLiteral("modelVersion")).toString();
            QVERIFY(!modelVersionBefore.isEmpty());
        }

        struct PositiveTarget {
            qint64 itemId = 0;
            QString path;
            QString contextEventId;
            QString activityDigest;
        };
        std::vector<PositiveTarget> positiveTargets;
        positiveTargets.reserve(15);

        for (int round = 0; round < 3; ++round) {
            const QString contextId = QStringLiteral("ctx-native-promote-%1").arg(round + 1);
            const QString digest = QStringLiteral("digest-native-promote-%1").arg(round + 1);

            QJsonObject params;
            params[QStringLiteral("query")] = QStringLiteral("report");
            params[QStringLiteral("limit")] = 20;
            params[QStringLiteral("contextEventId")] = contextId;
            params[QStringLiteral("activityDigest")] = digest;

            const QJsonObject response = harness.request(QStringLiteral("search"), params, 12000);
            QVERIFY(bs::test::isResponse(response));
            const QJsonArray results =
                bs::test::resultPayload(response).value(QStringLiteral("results")).toArray();
            QVERIFY(results.size() >= 15);

            const int takeCount = std::min(5, static_cast<int>(results.size()));
            for (int i = 0; i < takeCount; ++i) {
                const QJsonObject row = results.at(i).toObject();
                PositiveTarget target;
                target.itemId = row.value(QStringLiteral("itemId")).toInteger(0);
                target.path = row.value(QStringLiteral("path")).toString();
                target.contextEventId = contextId;
                target.activityDigest = digest;
                if (target.itemId > 0 && !target.path.isEmpty()) {
                    positiveTargets.push_back(std::move(target));
                }
            }
        }

        QVERIFY(positiveTargets.size() >= 15);

        for (int i = 0; i < 15; ++i) {
            const PositiveTarget& target = positiveTargets.at(static_cast<size_t>(i));
            QJsonObject params;
            params[QStringLiteral("eventId")] = QStringLiteral("native-promote-open-%1").arg(i + 1);
            params[QStringLiteral("eventType")] = QStringLiteral("result_open");
            params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
            params[QStringLiteral("timestamp")] =
                static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
            params[QStringLiteral("itemId")] = target.itemId;
            params[QStringLiteral("itemPath")] = target.path;
            params[QStringLiteral("query")] = QStringLiteral("report");
            params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
            params[QStringLiteral("contextEventId")] = target.contextEventId;
            params[QStringLiteral("activityDigest")] = target.activityDigest;
            params[QStringLiteral("attributionConfidence")] = 1.0;

            const QJsonObject response =
                harness.request(QStringLiteral("record_behavior_event"), params, 10000);
            QVERIFY(bs::test::isResponse(response));
            const QJsonObject result = bs::test::resultPayload(response);
            QVERIFY(result.value(QStringLiteral("recorded")).toBool(false));
            QVERIFY(result.value(QStringLiteral("attributedPositive")).toBool(false));
        }

        QTest::qWait(1500);

        {
            const QJsonObject response = harness.request(QStringLiteral("trigger_learning_cycle"));
            QVERIFY(bs::test::isResponse(response));
            const QJsonObject result = bs::test::resultPayload(response);
            QVERIFY(result.value(QStringLiteral("promoted")).toBool(false));
            QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("promoted"));
            const QJsonObject learning = result.value(QStringLiteral("learning")).toObject();
            QCOMPARE(learning.value(QStringLiteral("activeBackend")).toString(),
                     QStringLiteral("native_sgd"));
            QCOMPARE(learning.value(QStringLiteral("lastCycleStatus")).toString(),
                     QStringLiteral("succeeded"));
            QCOMPARE(learning.value(QStringLiteral("lastCycleReason")).toString(),
                     QStringLiteral("promoted"));
        }

        {
            const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
            QVERIFY(bs::test::isResponse(response));
            const QJsonObject learning = bs::test::resultPayload(response)
                                             .value(QStringLiteral("learning"))
                                             .toObject();
            QVERIFY(!learning.isEmpty());
            modelVersionAfter = learning.value(QStringLiteral("modelVersion")).toString();
            QVERIFY(!modelVersionAfter.isEmpty());
            QVERIFY(modelVersionAfter != modelVersionBefore);
            QCOMPARE(learning.value(QStringLiteral("activeBackend")).toString(),
                     QStringLiteral("native_sgd"));
            QVERIFY(!learning.value(QStringLiteral("coreMlModelAvailable")).toBool(true));
            QVERIFY(learning.value(QStringLiteral("cyclesSucceeded")).toInt(0) >= 1);
            const QJsonArray recent = learning.value(QStringLiteral("recentLearningCycles")).toArray();
            QVERIFY(!recent.isEmpty());
            const QJsonObject latest = recent.first().toObject();
            QCOMPARE(latest.value(QStringLiteral("status")).toString(),
                     QStringLiteral("succeeded"));
            QCOMPARE(latest.value(QStringLiteral("reason")).toString(),
                     QStringLiteral("promoted"));
            QVERIFY(latest.value(QStringLiteral("promoted")).toBool(false));
        }
    }

    const std::optional<QString> rollbackVersion =
        readLearningModelStateValue(dbPath, QStringLiteral("rollback_version"));
    const std::optional<QString> activeVersion =
        readLearningModelStateValue(dbPath, QStringLiteral("active_version"));
    const std::optional<QString> activeBackend =
        readLearningModelStateValue(dbPath, QStringLiteral("active_backend"));
    const std::optional<QString> lastCycleStatus =
        readLearningModelStateValue(dbPath, QStringLiteral("last_cycle_status"));
    const std::optional<QString> lastCycleReason =
        readLearningModelStateValue(dbPath, QStringLiteral("last_cycle_reason"));

    QVERIFY(rollbackVersion.has_value());
    QVERIFY(activeVersion.has_value());
    QVERIFY(activeBackend.has_value());
    QVERIFY(lastCycleStatus.has_value());
    QVERIFY(lastCycleReason.has_value());
    QCOMPARE(rollbackVersion.value(), modelVersionBefore);
    QCOMPARE(activeVersion.value(), modelVersionAfter);
    QCOMPARE(activeBackend.value(), QStringLiteral("native_sgd"));
    QCOMPARE(lastCycleStatus.value(), QStringLiteral("succeeded"));
    QCOMPARE(lastCycleReason.value(), QStringLiteral("promoted"));
}

void TestQueryServiceM2Ipc::testLearningServingFallbackWithoutModels()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for fallback fixture seeding");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    for (int i = 0; i < 8; ++i) {
        const QString fileName = QStringLiteral("fallback-%1.md")
                                     .arg(i + 1, 2, 10, QLatin1Char('0'));
        auto itemIdOpt = seedItemWithChunks(
            fixtureStore,
            docsDir.path(),
            fileName,
            {
                QStringLiteral("fallback ranking fixture report %1").arg(i),
                QStringLiteral("corrupt model artifacts should not crash serving"),
            });
        QVERIFY2(itemIdOpt.has_value(), "Failed to seed fallback fixture item");
    }

    const QString invalidCoreMlBootstrapDir = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(invalidCoreMlBootstrapDir));
    {
        QFile invalidMarker(QDir(invalidCoreMlBootstrapDir).filePath(QStringLiteral("invalid.bin")));
        QVERIFY(invalidMarker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalidMarker.write("invalid-coreml-bootstrap");
        invalidMarker.close();
    }

    const QString invalidNativeWeightsPath = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/active/weights.json"));
    QVERIFY(QDir().mkpath(QFileInfo(invalidNativeWeightsPath).absolutePath()));
    {
        QFile invalidWeights(invalidNativeWeightsPath);
        QVERIFY(invalidWeights.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalidWeights.write("{ invalid_json ");
        invalidWeights.close();
    }

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for fallback model test");

    int fallbackMissingModelBefore = 0;
    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("blended_ranking");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("updated")).toBool(false));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        QVERIFY(!learning.isEmpty());
        QVERIFY(!learning.value(QStringLiteral("modelAvailable")).toBool(true));
        QCOMPARE(learning.value(QStringLiteral("activeBackend")).toString(), QStringLiteral("none"));
        fallbackMissingModelBefore = learning.value(QStringLiteral("fallbackMissingModel")).toInt(0);
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("fallback report");
        params[QStringLiteral("limit")] = 5;
        params[QStringLiteral("debug")] = true;

        const QJsonObject response = harness.request(QStringLiteral("search"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("results")).isArray());
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(!debugInfo.isEmpty());
        QVERIFY(!debugInfo.value(QStringLiteral("onlineRankerApplied")).toBool(true));
        QVERIFY(std::abs(debugInfo.value(QStringLiteral("onlineRankerDeltaTop10")).toDouble(0.0)) < 1e-9);
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        QVERIFY(!learning.isEmpty());
        QVERIFY(!learning.value(QStringLiteral("modelAvailable")).toBool(true));
        QCOMPARE(learning.value(QStringLiteral("activeBackend")).toString(), QStringLiteral("none"));
        const int fallbackMissingModelAfter =
            learning.value(QStringLiteral("fallbackMissingModel")).toInt(0);
        QVERIFY(fallbackMissingModelAfter > fallbackMissingModelBefore);
    }
}

void TestQueryServiceM2Ipc::testLearningIdleCycleReasonGates()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for idle-gate fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("idle-gates.md"),
        {
            QStringLiteral("idle scheduler gate test fixture"),
            QStringLiteral("cooldown and activity reason assertions"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed idle-gate fixture item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("idle-gates.md"));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for idle-gate test");

    auto makeEventParams = [&](const QString& eventId) {
        QJsonObject params;
        params[QStringLiteral("eventId")] = eventId;
        params[QStringLiteral("eventType")] = QStringLiteral("result_open");
        params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
        params[QStringLiteral("timestamp")] = static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        params[QStringLiteral("itemId")] = seededItemId;
        params[QStringLiteral("itemPath")] = seededPath;
        params[QStringLiteral("query")] = QStringLiteral("report");
        params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
        params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-idle-gates");
        params[QStringLiteral("activityDigest")] = QStringLiteral("digest-idle-gates");
        params[QStringLiteral("attributionConfidence")] = 1.0;
        return params;
    };

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("instrumentation_only");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }
    {
        const QJsonObject response = harness.request(
            QStringLiteral("record_behavior_event"),
            makeEventParams(QStringLiteral("idle-rollout-blocked")));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("idleCycleTriggered")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("idleCycleReason")).toString(),
                 QStringLiteral("rollout_mode_blocks_training"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = true;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }
    {
        const QJsonObject response = harness.request(
            QStringLiteral("record_behavior_event"),
            makeEventParams(QStringLiteral("idle-user-active")));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("idleCycleTriggered")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("idleCycleReason")).toString(),
                 QStringLiteral("user_recently_active"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }
    {
        const QJsonObject response = harness.request(QStringLiteral("trigger_learning_cycle"));
        QVERIFY(bs::test::isResponse(response));
    }
    {
        const QJsonObject response = harness.request(
            QStringLiteral("record_behavior_event"),
            makeEventParams(QStringLiteral("idle-cooldown")));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("idleCycleTriggered")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("idleCycleReason")).toString(),
                 QStringLiteral("cooldown_active"));
    }
}

void TestQueryServiceM2Ipc::testLearningPromotionRejectsOnRuntimeGate()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for runtime-gate reject fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("runtime-gate.md"),
        {
            QStringLiteral("runtime gate reject fixture"),
            QStringLiteral("forcing non-finite candidate evaluation"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed runtime-gate fixture item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("runtime-gate.md"));

    const QString invalidCoreMlBootstrapDir = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(invalidCoreMlBootstrapDir));
    {
        QFile invalidMarker(QDir(invalidCoreMlBootstrapDir).filePath(QStringLiteral("invalid.bin")));
        QVERIFY(invalidMarker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalidMarker.write("invalid-coreml-bootstrap");
        invalidMarker.close();
    }

    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerMinExamples"), QStringLiteral("40")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerEpochs"), QStringLiteral("8")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerLearningRate"), QStringLiteral("0.5")));

    sqlite3* db = fixtureStore.rawDb();
    QVERIFY(db != nullptr);
    const QString overflowFeatures = denseFeaturesJson(1e308);
    const double createdAt = static_cast<double>(QDateTime::currentSecsSinceEpoch() - 120);
    for (int i = 0; i < 80; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        QVERIFY(insertTrainingExample(db,
                                      QStringLiteral("runtime-gate-sample-%1").arg(i + 1),
                                      seededItemId,
                                      seededPath,
                                      label,
                                      overflowFeatures,
                                      createdAt + static_cast<double>(i)));
    }

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for runtime-gate reject test");

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("trigger_learning_cycle"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("promoted")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("candidate_stability_invalid_eval"));
        const QJsonObject learning = result.value(QStringLiteral("learning")).toObject();
        QCOMPARE(learning.value(QStringLiteral("lastCycleStatus")).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(learning.value(QStringLiteral("lastCycleReason")).toString(),
                 QStringLiteral("candidate_stability_invalid_eval"));
        QVERIFY(learning.value(QStringLiteral("cyclesRejected")).toInt(0) >= 1);
        const QJsonArray recent = learning.value(QStringLiteral("recentLearningCycles")).toArray();
        QVERIFY(!recent.isEmpty());
        const QJsonObject latest = recent.first().toObject();
        QCOMPARE(latest.value(QStringLiteral("status")).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(latest.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("candidate_stability_invalid_eval"));
        QVERIFY(!latest.value(QStringLiteral("promoted")).toBool(true));
    }
}

void TestQueryServiceM2Ipc::testLearningSchedulerReasonCounts()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for scheduler fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("scheduler.md"),
        {
            QStringLiteral("scheduler reason counts fixture"),
            QStringLiteral("timer-driven idle cycle bookkeeping"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed scheduler fixture item");

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.env.insert(QStringLiteral("BS_TEST_LEARNING_SCHEDULER_INTERVAL_MS"), QStringLiteral("200"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for scheduler test");

    auto fetchScheduler = [&]() -> QJsonObject {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        if (!bs::test::isResponse(response)) {
            return QJsonObject();
        }
        const QJsonObject learning =
            bs::test::resultPayload(response).value(QStringLiteral("learning")).toObject();
        if (learning.isEmpty()) {
            return QJsonObject();
        }
        return learning.value(QStringLiteral("scheduler")).toObject();
    };

    auto waitForTicksAtLeast = [&](int targetTicks, int timeoutMs) -> QJsonObject {
        QElapsedTimer timer;
        timer.start();
        QJsonObject scheduler;
        while (timer.elapsed() < timeoutMs) {
            scheduler = fetchScheduler();
            if (scheduler.value(QStringLiteral("ticks")).toInt(0) >= targetTicks) {
                return scheduler;
            }
            QTest::qWait(50);
        }
        return scheduler;
    };

    QJsonObject scheduler = waitForTicksAtLeast(1, 5000);
    QVERIFY(!scheduler.isEmpty());
    QVERIFY(scheduler.value(QStringLiteral("enabled")).toBool(false));
    QCOMPARE(scheduler.value(QStringLiteral("intervalMs")).toInt(0), 200);
    QVERIFY(scheduler.value(QStringLiteral("ticks")).toInt(0) >= 1);
    {
        const QJsonObject reasonCounts = scheduler.value(QStringLiteral("reasonCounts")).toObject();
        QVERIFY(reasonCounts.value(QStringLiteral("learning_disabled")).toInt(0) >= 1);
    }

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("instrumentation_only");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    const int baselineTicks = scheduler.value(QStringLiteral("ticks")).toInt(0);
    scheduler = waitForTicksAtLeast(baselineTicks + 1, 5000);
    QVERIFY(!scheduler.isEmpty());
    QVERIFY(scheduler.value(QStringLiteral("ticks")).toInt(0) >= baselineTicks + 1);
    {
        const QJsonObject reasonCounts = scheduler.value(QStringLiteral("reasonCounts")).toObject();
        QVERIFY(reasonCounts.value(QStringLiteral("rollout_mode_blocks_training")).toInt(0) >= 1);
    }
}

void TestQueryServiceM2Ipc::testLearningSchedulerTransitionSequence()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.env.insert(QStringLiteral("BS_TEST_LEARNING_SCHEDULER_INTERVAL_MS"), QStringLiteral("200"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for scheduler transition test");

    auto fetchScheduler = [&]() -> QJsonObject {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        if (!bs::test::isResponse(response)) {
            return QJsonObject();
        }
        const QJsonObject learning =
            bs::test::resultPayload(response).value(QStringLiteral("learning")).toObject();
        if (learning.isEmpty()) {
            return QJsonObject();
        }
        return learning.value(QStringLiteral("scheduler")).toObject();
    };

    auto ticksOf = [](const QJsonObject& scheduler) -> qint64 {
        return scheduler.value(QStringLiteral("ticks")).toInteger(0);
    };
    auto lastTickAtMsOf = [](const QJsonObject& scheduler) -> qint64 {
        return scheduler.value(QStringLiteral("lastTickAtMs")).toInteger(0);
    };
    auto reasonCountOf = [](const QJsonObject& scheduler, const QString& reason) -> qint64 {
        return scheduler.value(QStringLiteral("reasonCounts")).toObject()
            .value(reason).toInteger(0);
    };
    auto sumReasonCounts = [](const QJsonObject& scheduler) -> qint64 {
        qint64 total = 0;
        const QJsonObject reasonCounts = scheduler.value(QStringLiteral("reasonCounts")).toObject();
        for (auto it = reasonCounts.constBegin(); it != reasonCounts.constEnd(); ++it) {
            total += it.value().toInteger(0);
        }
        return total;
    };

    auto waitForTicksAtLeast = [&](qint64 targetTicks, int timeoutMs) -> QJsonObject {
        QElapsedTimer timer;
        timer.start();
        QJsonObject scheduler;
        while (timer.elapsed() < timeoutMs) {
            scheduler = fetchScheduler();
            if (ticksOf(scheduler) >= targetTicks) {
                return scheduler;
            }
            QTest::qWait(50);
        }
        return scheduler;
    };

    auto waitForReasonCountIncrease = [&](const QString& reason,
                                          qint64 baseline,
                                          int timeoutMs) -> QJsonObject {
        QElapsedTimer timer;
        timer.start();
        QJsonObject scheduler;
        while (timer.elapsed() < timeoutMs) {
            scheduler = fetchScheduler();
            if (reasonCountOf(scheduler, reason) > baseline) {
                return scheduler;
            }
            QTest::qWait(50);
        }
        return scheduler;
    };

    QJsonObject scheduler = waitForTicksAtLeast(1, 5000);
    QVERIFY(!scheduler.isEmpty());
    QVERIFY(ticksOf(scheduler) >= 1);
    QVERIFY(reasonCountOf(scheduler, QStringLiteral("learning_disabled")) >= 1);
    QCOMPARE(sumReasonCounts(scheduler), ticksOf(scheduler));

    const qint64 baseTicks = ticksOf(scheduler);
    const qint64 baseLastTickAt = lastTickAtMsOf(scheduler);
    const qint64 baseRolloutBlocked =
        reasonCountOf(scheduler, QStringLiteral("rollout_mode_blocks_training"));

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("instrumentation_only");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    scheduler = waitForReasonCountIncrease(QStringLiteral("rollout_mode_blocks_training"),
                                           baseRolloutBlocked,
                                           5000);
    QVERIFY(!scheduler.isEmpty());
    QVERIFY(ticksOf(scheduler) > baseTicks);
    QVERIFY(lastTickAtMsOf(scheduler) >= baseLastTickAt);
    QVERIFY(reasonCountOf(scheduler, QStringLiteral("rollout_mode_blocks_training"))
            > baseRolloutBlocked);
    QCOMPARE(sumReasonCounts(scheduler), ticksOf(scheduler));

    const qint64 ticksAfterRolloutBlocked = ticksOf(scheduler);
    const qint64 lastTickAfterRolloutBlocked = lastTickAtMsOf(scheduler);
    const qint64 baseNotEnough =
        reasonCountOf(scheduler, QStringLiteral("not_enough_training_examples"));

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    scheduler = waitForReasonCountIncrease(QStringLiteral("not_enough_training_examples"),
                                           baseNotEnough,
                                           5000);
    QVERIFY(!scheduler.isEmpty());
    QVERIFY(ticksOf(scheduler) > ticksAfterRolloutBlocked);
    QVERIFY(lastTickAtMsOf(scheduler) >= lastTickAfterRolloutBlocked);
    QVERIFY(reasonCountOf(scheduler, QStringLiteral("not_enough_training_examples"))
            > baseNotEnough);
    QCOMPARE(sumReasonCounts(scheduler), ticksOf(scheduler));

    const qint64 ticksAfterNotEnough = ticksOf(scheduler);
    const qint64 lastTickAfterNotEnough = lastTickAtMsOf(scheduler);
    const qint64 baseLearningDisabled =
        reasonCountOf(scheduler, QStringLiteral("learning_disabled"));

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = false;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    scheduler = waitForReasonCountIncrease(QStringLiteral("learning_disabled"),
                                           baseLearningDisabled,
                                           5000);
    QVERIFY(!scheduler.isEmpty());
    QVERIFY(ticksOf(scheduler) > ticksAfterNotEnough);
    QVERIFY(lastTickAtMsOf(scheduler) >= lastTickAfterNotEnough);
    QVERIFY(reasonCountOf(scheduler, QStringLiteral("learning_disabled"))
            > baseLearningDisabled);
    QCOMPARE(sumReasonCounts(scheduler), ticksOf(scheduler));
}

void TestQueryServiceM2Ipc::testSetLearningConsentRejectsInvalidRolloutMode()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for invalid-rollout test");

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("definitely_invalid_mode");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isError(response));
        const QJsonObject error = bs::test::errorPayload(response);
        QCOMPARE(error.value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
        QCOMPARE(error.value(QStringLiteral("message")).toString(),
                 QStringLiteral("invalid_rollout_mode"));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        QVERIFY(!learning.isEmpty());
        QVERIFY(!learning.value(QStringLiteral("learningEnabled")).toBool(true));
        QCOMPARE(learning.value(QStringLiteral("onlineRankerRolloutMode")).toString(),
                 QStringLiteral("instrumentation_only"));
    }
}

void TestQueryServiceM2Ipc::testLearningCycleHistoryBoundedViaRpc()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for bounded-history fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("bounded-history.md"),
        {
            QStringLiteral("bounded learning cycle history fixture"),
            QStringLiteral("repeated rpc-triggered cycles should remain bounded"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed bounded-history fixture item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("bounded-history.md"));

    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerRecentCycleHistoryLimit"),
                                    QStringLiteral("5")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerMinExamples"),
                                    QStringLiteral("20")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerNegativeSampleRatio"),
                                    QStringLiteral("1.0")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerMaxTrainingBatchSize"),
                                    QStringLiteral("160")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionGateMinPositives"),
                                    QStringLiteral("1")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionMinAttributedRate"),
                                    QStringLiteral("0.0")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionMinContextDigestRate"),
                                    QStringLiteral("0.0")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionLatencyUsMax"),
                                    QStringLiteral("1000000")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionLatencyRegressionPctMax"),
                                    QStringLiteral("1000")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionPredictionFailureRateMax"),
                                    QStringLiteral("1.0")));
    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerPromotionSaturationRateMax"),
                                    QStringLiteral("1.0")));

    sqlite3* db = fixtureStore.rawDb();
    QVERIFY(db != nullptr);
    const double createdAt = static_cast<double>(QDateTime::currentSecsSinceEpoch() - 120);
    for (int i = 0; i < 320; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        const double featureBase = label > 0 ? 0.82 : 0.18;
        QVERIFY(insertTrainingExample(
            db,
            QStringLiteral("history-sample-%1").arg(i + 1),
            seededItemId,
            seededPath,
            label,
            denseFeaturesJson(featureBase),
            createdAt + static_cast<double>(i)));
    }

    const QString invalidCoreMlBootstrapDir = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(invalidCoreMlBootstrapDir));
    {
        QFile invalidMarker(QDir(invalidCoreMlBootstrapDir).filePath(QStringLiteral("invalid.bin")));
        QVERIFY(invalidMarker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalidMarker.write("invalid-coreml-bootstrap");
        invalidMarker.close();
    }

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for bounded-history test");

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    QString lastReason;
    for (int i = 0; i < 12; ++i) {
        QJsonObject params;
        params[QStringLiteral("manual")] = false;
        const QJsonObject response =
            harness.request(QStringLiteral("trigger_learning_cycle"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.contains(QStringLiteral("promoted")));
        QVERIFY(result.value(QStringLiteral("reason")).isString());
        lastReason = result.value(QStringLiteral("reason")).toString();
        QVERIFY(!lastReason.isEmpty());
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        QVERIFY(!learning.isEmpty());
        QVERIFY(learning.value(QStringLiteral("cyclesRun")).toInt(0) >= 12);
        QCOMPARE(learning.value(QStringLiteral("recentLearningCyclesLimit")).toInt(0), 5);

        const QJsonArray recent = learning.value(QStringLiteral("recentLearningCycles")).toArray();
        QCOMPARE(recent.size(), 5);
        QCOMPARE(recent.first().toObject().value(QStringLiteral("reason")).toString(), lastReason);
        for (int i = 1; i < recent.size(); ++i) {
            const qint64 prevIndex =
                recent.at(i - 1).toObject().value(QStringLiteral("cycleIndex")).toInteger(0);
            const qint64 currentIndex =
                recent.at(i).toObject().value(QStringLiteral("cycleIndex")).toInteger(0);
            QVERIFY(prevIndex >= currentIndex);
        }
    }
}

void TestQueryServiceM2Ipc::testRecordBehaviorEventPrivacyExclusionsBlockAttribution()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for privacy-exclusion fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("privacy.md"),
        {
            QStringLiteral("privacy exclusion fixture"),
            QStringLiteral("secure and private flags must not create labels"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed privacy-exclusion fixture item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("privacy.md"));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for privacy-exclusion test");

    auto readPositiveAndEvents = [&]() -> std::pair<int, int> {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        if (!bs::test::isResponse(response)) {
            return {0, 0};
        }
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        if (learning.isEmpty()) {
            return {0, 0};
        }
        const QJsonObject attribution =
            learning.value(QStringLiteral("attributionMetrics")).toObject();
        const QJsonObject coverage =
            learning.value(QStringLiteral("behaviorCoverageMetrics")).toObject();
        return {attribution.value(QStringLiteral("positiveExamples")).toInt(0),
                coverage.value(QStringLiteral("events")).toInt(0)};
    };

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = false;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    const auto before = readPositiveAndEvents();

    auto makeParams = [&](const QString& eventId) {
        QJsonObject params;
        params[QStringLiteral("eventId")] = eventId;
        params[QStringLiteral("eventType")] = QStringLiteral("result_open");
        params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
        params[QStringLiteral("timestamp")] =
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        params[QStringLiteral("itemId")] = seededItemId;
        params[QStringLiteral("itemPath")] = seededPath;
        params[QStringLiteral("query")] = QStringLiteral("privacy");
        params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
        params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-privacy");
        params[QStringLiteral("activityDigest")] = QStringLiteral("digest-privacy");
        params[QStringLiteral("attributionConfidence")] = 1.0;
        return params;
    };

    {
        QJsonObject params = makeParams(QStringLiteral("privacy-secure"));
        QJsonObject privacyFlags;
        privacyFlags[QStringLiteral("secureInput")] = true;
        privacyFlags[QStringLiteral("privateContext")] = false;
        privacyFlags[QStringLiteral("denylistedApp")] = false;
        privacyFlags[QStringLiteral("redacted")] = false;
        params[QStringLiteral("privacyFlags")] = privacyFlags;

        const QJsonObject response =
            harness.request(QStringLiteral("record_behavior_event"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("recorded")).toBool(true));
        QVERIFY(result.value(QStringLiteral("filteredOut")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("attributedPositive")).toBool(true));
    }

    {
        QJsonObject params = makeParams(QStringLiteral("privacy-private"));
        QJsonObject privacyFlags;
        privacyFlags[QStringLiteral("secureInput")] = false;
        privacyFlags[QStringLiteral("privateContext")] = true;
        privacyFlags[QStringLiteral("denylistedApp")] = false;
        privacyFlags[QStringLiteral("redacted")] = false;
        params[QStringLiteral("privacyFlags")] = privacyFlags;

        const QJsonObject response =
            harness.request(QStringLiteral("record_behavior_event"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("recorded")).toBool(true));
        QVERIFY(result.value(QStringLiteral("filteredOut")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("attributedPositive")).toBool(true));
    }

    const auto after = readPositiveAndEvents();
    QCOMPARE(after.first, before.first);
    QCOMPARE(after.second, before.second);
}

void TestQueryServiceM2Ipc::testRecordBehaviorEventDuplicateIdIgnored()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for duplicate-event fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("duplicate-event.md"),
        {
            QStringLiteral("duplicate behavior event fixture"),
            QStringLiteral("same event id must be ignored on replay"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed duplicate-event fixture item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("duplicate-event.md"));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for duplicate-event test");

    auto readPositiveAndEvents = [&]() -> std::pair<int, int> {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        if (!bs::test::isResponse(response)) {
            return {0, 0};
        }
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        if (learning.isEmpty()) {
            return {0, 0};
        }
        const QJsonObject attribution =
            learning.value(QStringLiteral("attributionMetrics")).toObject();
        const QJsonObject coverage =
            learning.value(QStringLiteral("behaviorCoverageMetrics")).toObject();
        return {attribution.value(QStringLiteral("positiveExamples")).toInt(0),
                coverage.value(QStringLiteral("events")).toInt(0)};
    };

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = true;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    const auto before = readPositiveAndEvents();

    QJsonObject params;
    params[QStringLiteral("eventId")] = QStringLiteral("duplicate-event-1");
    params[QStringLiteral("eventType")] = QStringLiteral("result_open");
    params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
    params[QStringLiteral("timestamp")] =
        static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
    params[QStringLiteral("itemId")] = seededItemId;
    params[QStringLiteral("itemPath")] = seededPath;
    params[QStringLiteral("query")] = QStringLiteral("duplicate");
    params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
    params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-duplicate");
    params[QStringLiteral("activityDigest")] = QStringLiteral("digest-duplicate");
    params[QStringLiteral("attributionConfidence")] = 1.0;

    {
        const QJsonObject response =
            harness.request(QStringLiteral("record_behavior_event"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("recorded")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("filteredOut")).toBool(true));
        QVERIFY(result.value(QStringLiteral("attributedPositive")).toBool(false));
    }

    const auto afterFirst = readPositiveAndEvents();
    QVERIFY(afterFirst.first >= before.first + 1);
    QVERIFY(afterFirst.second >= before.second + 1);

    {
        const QJsonObject response =
            harness.request(QStringLiteral("record_behavior_event"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("recorded")).toBool(true));
        QVERIFY(result.value(QStringLiteral("filteredOut")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("attributedPositive")).toBool(true));
    }

    const auto afterSecond = readPositiveAndEvents();
    QCOMPARE(afterSecond.first, afterFirst.first);
    QCOMPARE(afterSecond.second, afterFirst.second);
}

void TestQueryServiceM2Ipc::testDuplicateReplayDoesNotInflateQueuesOrScheduler()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for duplicate-replay fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    auto seededItemIdOpt = seedItemWithChunks(
        fixtureStore,
        docsDir.path(),
        QStringLiteral("duplicate-replay.md"),
        {
            QStringLiteral("duplicate replay fixture"),
            QStringLiteral("replayed event ids should not inflate labels or queues"),
        });
    QVERIFY2(seededItemIdOpt.has_value(), "Failed to seed duplicate-replay fixture item");
    const qint64 seededItemId = seededItemIdOpt.value();
    const QString seededPath = QDir(docsDir.path()).filePath(QStringLiteral("duplicate-replay.md"));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    // Keep scheduler quiescent during this test so reason-count assertions are deterministic.
    launch.env.insert(QStringLiteral("BS_TEST_LEARNING_SCHEDULER_INTERVAL_MS"), QStringLiteral("60000"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for duplicate-replay test");

    auto readLearning = [&]() -> QJsonObject {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        if (!bs::test::isResponse(response)) {
            return QJsonObject();
        }
        return bs::test::resultPayload(response).value(QStringLiteral("learning")).toObject();
    };

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = true;
        params[QStringLiteral("onlineRankerRolloutMode")] = QStringLiteral("shadow_training");
        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
    }

    const QJsonObject beforeLearning = readLearning();
    QVERIFY(!beforeLearning.isEmpty());
    const int beforePositives =
        beforeLearning.value(QStringLiteral("attributionMetrics")).toObject()
            .value(QStringLiteral("positiveExamples")).toInt(0);
    const int beforeEvents =
        beforeLearning.value(QStringLiteral("behaviorCoverageMetrics")).toObject()
            .value(QStringLiteral("events")).toInt(0);
    const int beforePending = beforeLearning.value(QStringLiteral("pendingExamples")).toInt(0);
    const int beforeSchedulerTicks =
        beforeLearning.value(QStringLiteral("scheduler")).toObject()
            .value(QStringLiteral("ticks")).toInt(0);
    const QJsonObject beforeReasonCounts =
        beforeLearning.value(QStringLiteral("scheduler")).toObject()
            .value(QStringLiteral("reasonCounts")).toObject();

    QJsonObject params;
    params[QStringLiteral("eventId")] = QStringLiteral("duplicate-replay-1");
    params[QStringLiteral("eventType")] = QStringLiteral("result_open");
    params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
    params[QStringLiteral("timestamp")] =
        static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
    params[QStringLiteral("itemId")] = seededItemId;
    params[QStringLiteral("itemPath")] = seededPath;
    params[QStringLiteral("query")] = QStringLiteral("duplicate replay");
    params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
    params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-duplicate-replay");
    params[QStringLiteral("activityDigest")] = QStringLiteral("digest-duplicate-replay");
    params[QStringLiteral("attributionConfidence")] = 1.0;

    {
        const QJsonObject response =
            harness.request(QStringLiteral("record_behavior_event"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("recorded")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("filteredOut")).toBool(true));
        QVERIFY(result.value(QStringLiteral("attributedPositive")).toBool(false));
    }

    const QJsonObject afterFirstLearning = readLearning();
    QVERIFY(!afterFirstLearning.isEmpty());
    const int afterFirstPositives =
        afterFirstLearning.value(QStringLiteral("attributionMetrics")).toObject()
            .value(QStringLiteral("positiveExamples")).toInt(0);
    const int afterFirstEvents =
        afterFirstLearning.value(QStringLiteral("behaviorCoverageMetrics")).toObject()
            .value(QStringLiteral("events")).toInt(0);
    const int afterFirstPending = afterFirstLearning.value(QStringLiteral("pendingExamples")).toInt(0);
    QVERIFY(afterFirstPositives >= beforePositives + 1);
    QVERIFY(afterFirstEvents >= beforeEvents + 1);
    QVERIFY(afterFirstPending >= beforePending + 1);

    for (int i = 0; i < 10; ++i) {
        const QJsonObject response =
            harness.request(QStringLiteral("record_behavior_event"), params, 12000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("recorded")).toBool(true));
        QVERIFY(result.value(QStringLiteral("filteredOut")).toBool(false));
        QVERIFY(!result.value(QStringLiteral("attributedPositive")).toBool(true));
    }

    const QJsonObject afterReplayLearning = readLearning();
    QVERIFY(!afterReplayLearning.isEmpty());
    const int afterReplayPositives =
        afterReplayLearning.value(QStringLiteral("attributionMetrics")).toObject()
            .value(QStringLiteral("positiveExamples")).toInt(0);
    const int afterReplayEvents =
        afterReplayLearning.value(QStringLiteral("behaviorCoverageMetrics")).toObject()
            .value(QStringLiteral("events")).toInt(0);
    const int afterReplayPending =
        afterReplayLearning.value(QStringLiteral("pendingExamples")).toInt(0);
    QCOMPARE(afterReplayPositives, afterFirstPositives);
    QCOMPARE(afterReplayEvents, afterFirstEvents);
    QCOMPARE(afterReplayPending, afterFirstPending);

    const QJsonObject scheduler = afterReplayLearning.value(QStringLiteral("scheduler")).toObject();
    QCOMPARE(scheduler.value(QStringLiteral("ticks")).toInt(0), beforeSchedulerTicks);
    QCOMPARE(scheduler.value(QStringLiteral("reasonCounts")).toObject(), beforeReasonCounts);
}

void TestQueryServiceM2Ipc::testOnlineRankerServingRespectsRolloutModes()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsDir.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to open query DB for rollout-serving fixture");
    bs::SQLiteStore fixtureStore = std::move(storeOpt.value());

    for (int i = 0; i < 10; ++i) {
        const QString fileName = QStringLiteral("rollout-%1.md")
                                     .arg(i + 1, 2, 10, QLatin1Char('0'));
        auto itemIdOpt = seedItemWithChunks(
            fixtureStore,
            docsDir.path(),
            fileName,
            {
                QStringLiteral("rollout serving fixture %1").arg(i),
                QStringLiteral("blended ranking should apply online ranker only in serving mode"),
            });
        QVERIFY2(itemIdOpt.has_value(), "Failed to seed rollout-serving fixture item");
    }

    QVERIFY(fixtureStore.setSetting(QStringLiteral("onlineRankerBlendAlpha"),
                                    QStringLiteral("0.05")));

    const QString invalidCoreMlBootstrapDir = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(invalidCoreMlBootstrapDir));
    {
        QFile invalidMarker(QDir(invalidCoreMlBootstrapDir).filePath(QStringLiteral("invalid.bin")));
        QVERIFY(invalidMarker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalidMarker.write("invalid-coreml-bootstrap");
        invalidMarker.close();
    }

    const QString nativeWeightsPath = QDir(dataDir).filePath(
        QStringLiteral("models/online-ranker-v1/active/weights.json"));
    QVERIFY(QDir().mkpath(QFileInfo(nativeWeightsPath).absolutePath()));
    {
        QJsonArray weights;
        for (int i = 0; i < 13; ++i) {
            weights.append(0.9);
        }
        QJsonObject model;
        model[QStringLiteral("version")] = QStringLiteral("test-native-rollout");
        model[QStringLiteral("bias")] = 3.0;
        model[QStringLiteral("weights")] = weights;

        QFile weightsFile(nativeWeightsPath);
        QVERIFY(weightsFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        weightsFile.write(QJsonDocument(model).toJson(QJsonDocument::Compact));
        weightsFile.close();
    }

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDINGS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_EMBEDDING_DIMS"), QStringLiteral("24"));
    launch.env.insert(QStringLiteral("BS_TEST_FAKE_FAST_EMBEDDING_DIMS"), QStringLiteral("16"));
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 12000;
    QVERIFY2(harness.start(launch), "Failed to start query service for rollout-serving test");

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject learning = bs::test::resultPayload(response)
                                         .value(QStringLiteral("learning"))
                                         .toObject();
        QVERIFY(!learning.isEmpty());
        QVERIFY(learning.value(QStringLiteral("modelAvailable")).toBool(false));
        QCOMPARE(learning.value(QStringLiteral("activeBackend")).toString(),
                 QStringLiteral("native_sgd"));
    }

    auto runDebugSearch = [&](const QString& rolloutMode) -> QJsonObject {
        {
            QJsonObject params;
            params[QStringLiteral("behaviorStreamEnabled")] = true;
            params[QStringLiteral("learningEnabled")] = true;
            params[QStringLiteral("learningPauseOnUserInput")] = false;
            params[QStringLiteral("onlineRankerRolloutMode")] = rolloutMode;
            const QJsonObject response =
                harness.request(QStringLiteral("set_learning_consent"), params);
            if (!bs::test::isResponse(response)) {
                return QJsonObject();
            }
        }

        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("rollout serving");
        params[QStringLiteral("limit")] = 8;
        params[QStringLiteral("debug")] = true;
        const QJsonObject response = harness.request(QStringLiteral("search"), params, 12000);
        if (!bs::test::isResponse(response)) {
            return QJsonObject();
        }
        return bs::test::resultPayload(response).value(QStringLiteral("debugInfo")).toObject();
    };

    const QJsonObject instrumentationDebug =
        runDebugSearch(QStringLiteral("instrumentation_only"));
    QVERIFY(!instrumentationDebug.isEmpty());
    QCOMPARE(instrumentationDebug.value(QStringLiteral("onlineRankerRolloutMode")).toString(),
             QStringLiteral("instrumentation_only"));
    QVERIFY(!instrumentationDebug.value(QStringLiteral("onlineRankerServingAllowed")).toBool(true));
    QVERIFY(!instrumentationDebug.value(QStringLiteral("onlineRankerApplied")).toBool(true));
    QVERIFY(std::abs(instrumentationDebug.value(QStringLiteral("onlineRankerDeltaTop10"))
                         .toDouble(0.0))
            < 1e-9);

    const QJsonObject shadowDebug =
        runDebugSearch(QStringLiteral("shadow_training"));
    QVERIFY(!shadowDebug.isEmpty());
    QCOMPARE(shadowDebug.value(QStringLiteral("onlineRankerRolloutMode")).toString(),
             QStringLiteral("shadow_training"));
    QVERIFY(!shadowDebug.value(QStringLiteral("onlineRankerServingAllowed")).toBool(true));
    QVERIFY(!shadowDebug.value(QStringLiteral("onlineRankerApplied")).toBool(true));
    QVERIFY(std::abs(shadowDebug.value(QStringLiteral("onlineRankerDeltaTop10"))
                         .toDouble(0.0))
            < 1e-9);

    const QJsonObject blendedDebug =
        runDebugSearch(QStringLiteral("blended_ranking"));
    QVERIFY(!blendedDebug.isEmpty());
    QCOMPARE(blendedDebug.value(QStringLiteral("onlineRankerRolloutMode")).toString(),
             QStringLiteral("blended_ranking"));
    QVERIFY(blendedDebug.value(QStringLiteral("onlineRankerServingAllowed")).toBool(false));
    QVERIFY(blendedDebug.value(QStringLiteral("onlineRankerApplied")).toBool(false));
    QVERIFY(std::abs(blendedDebug.value(QStringLiteral("onlineRankerDeltaTop10"))
                         .toDouble(0.0))
            > 1e-6);
}

QTEST_MAIN(TestQueryServiceM2Ipc)
#include "test_query_service_m2_ipc.moc"
