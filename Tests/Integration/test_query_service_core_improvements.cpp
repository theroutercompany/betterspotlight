#include <QtTest/QtTest>

#include "core/ipc/message.h"
#include "core/ipc/service_base.h"
#include "core/ipc/socket_client.h"
#include "core/ipc/socket_server.h"
#include "core/index/sqlite_store.h"
#include "core/shared/chunk.h"
#include "threaded_socket_server.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sqlite3.h>

namespace {

QString findQueryBinary()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString name = QStringLiteral("betterspotlight-query");
    const QStringList candidates = {
        QDir(appDir).filePath(name),
        QDir(appDir).filePath(QStringLiteral("../src/services/query/") + name),
        QDir(appDir).filePath(QStringLiteral("../../src/services/query/") + name),
        QDir(appDir).filePath(QStringLiteral("../../../src/services/query/") + name),
        QDir(appDir).filePath(QStringLiteral("../bin/") + name),
        QDir(appDir).filePath(QStringLiteral("../../bin/") + name),
    };

    for (const QString& candidate : candidates) {
        QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) {
            return info.canonicalFilePath();
        }
    }

    return QStandardPaths::findExecutable(name);
}

bool waitForQueryConnection(bs::SocketClient& client, const QString& socketPath, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (client.connectToServer(socketPath, 100)) {
            return true;
        }
        QTest::qWait(25);
    }
    return false;
}

std::optional<int64_t> upsertItem(
    bs::SQLiteStore& store,
    const QString& path,
    const QString& extension,
    bs::ItemKind kind,
    const QString& content)
{
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    QFileInfo info(path);
    auto itemId = store.upsertItem(
        path,
        info.fileName(),
        extension,
        kind,
        static_cast<int64_t>(content.size() > 0 ? content.size() : 1),
        now,
        now,
        QString(),
        QStringLiteral("normal"),
        info.path());
    if (!itemId.has_value()) {
        return std::nullopt;
    }

    bs::Chunk chunk;
    chunk.chunkId = bs::computeChunkId(path, 0);
    chunk.filePath = path;
    chunk.chunkIndex = 0;
    chunk.content = content;
    chunk.byteOffset = 0;
    const std::vector<bs::Chunk> chunks = {chunk};
    if (!store.insertChunks(itemId.value(), info.fileName(), path, chunks)) {
        return std::nullopt;
    }
    return itemId;
}

QJsonObject sendOrFail(bs::SocketClient& client,
                       const QString& method,
                       const QJsonObject& params = {},
                       int timeoutMs = 3000)
{
    auto response = client.sendRequest(method, params, timeoutMs);
    if (!response.has_value()) {
        return QJsonObject();
    }
    return response.value();
}

void sanitizeChildEnvironment(QProcessEnvironment& env)
{
    const QStringList keys = env.keys();
    for (const QString& key : keys) {
        if (key.startsWith(QStringLiteral("BS_TEST_"))) {
            env.remove(key);
        }
    }

    // Keep test behavior deterministic regardless of caller environment.
    env.remove(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"));
}

void assertFiniteDoubleValue(const QJsonObject& object,
                             const QString& key,
                             double expected)
{
    const QJsonValue value = object.value(key);
    QVERIFY2(value.isDouble(),
             qPrintable(QStringLiteral("Expected numeric runtime value for %1").arg(key)));
    const double actual = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    QVERIFY2(std::isfinite(actual),
             qPrintable(QStringLiteral("Expected finite runtime value for %1").arg(key)));
    QVERIFY2(std::abs(actual - expected) < 1e-9,
             qPrintable(QStringLiteral("Unexpected runtime value for %1: %2 expected %3")
                            .arg(key)
                            .arg(actual)
                            .arg(expected)));
}

} // namespace

class TestQueryServiceCoreImprovements : public QObject {
    Q_OBJECT

private slots:
    void testCoreBehaviorViaIpc();
};

void TestQueryServiceCoreImprovements::testCoreBehaviorViaIpc()
{
    QTemporaryDir tempHome;
    QVERIFY2(tempHome.isValid(), "Failed to create temporary HOME directory");
    QTemporaryDir tempSocketDir(QStringLiteral("/tmp/bs-qcore-XXXXXX"));
    QVERIFY2(tempSocketDir.isValid(), "Failed to create temporary socket directory");

    const QString dataDir = QDir(tempHome.path())
                                .filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to initialize test SQLite store");
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString docsDir = QDir(tempHome.path()).filePath(QStringLiteral("Documents"));
    QVERIFY(QDir().mkpath(docsDir));
    QVERIFY(store.setSetting(QStringLiteral("activeVectorGeneration"), QStringLiteral("v2")));
    QVERIFY(store.setSetting(QStringLiteral("targetVectorGeneration"), QStringLiteral("v2")));
    QVERIFY(store.setSetting(QStringLiteral("queryRouterMinConfidence"), QStringLiteral("nan")));
    QVERIFY(store.setSetting(QStringLiteral("bm25WeightName"), QStringLiteral("inf")));
    QVERIFY(store.setSetting(QStringLiteral("bm25WeightPath"), QStringLiteral("-inf")));
    QVERIFY(store.setSetting(QStringLiteral("bm25WeightContent"), QStringLiteral("nan")));
    QVERIFY(store.setSetting(QStringLiteral("mergeLexicalWeightNaturalLanguageStrong"),
                             QStringLiteral("nan")));
    QVERIFY(store.setSetting(QStringLiteral("mergeSemanticWeightNaturalLanguageStrong"),
                             QStringLiteral("nan")));
    QVERIFY(store.setSetting(QStringLiteral("onlineRankerBlendAlpha"), QStringLiteral("nan")));
    {
        sqlite3* db = nullptr;
        QVERIFY(sqlite3_open_v2(dbPath.toUtf8().constData(),
                                &db,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                                nullptr) == SQLITE_OK);
        const auto closeDb = qScopeGuard([&]() {
            if (db) {
                sqlite3_close(db);
            }
        });
        const char* sql = R"(
            INSERT INTO vector_generation_state (
                generation_id, model_id, dimensions, provider, state, progress_pct, is_active, updated_at
            ) VALUES ('v2', 'bge-large-en-v1.5-f32', 1024, 'cpu', 'active', 100.0, 1, strftime('%s','now'))
            ON CONFLICT(generation_id) DO UPDATE SET
                model_id = excluded.model_id,
                dimensions = excluded.dimensions,
                provider = excluded.provider,
                state = excluded.state,
                progress_pct = excluded.progress_pct,
                is_active = excluded.is_active,
                updated_at = excluded.updated_at;
            UPDATE vector_generation_state SET is_active = 0 WHERE generation_id != 'v2';
        )";
        char* errMsg = nullptr;
        const int execRc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        QVERIFY2(execRc == SQLITE_OK, errMsg ? errMsg : "Failed to seed vector_generation_state");
        sqlite3_free(errMsg);
    }
    {
        QFile vectorIndex(QDir(dataDir).filePath(QStringLiteral("vectors-v2.hnsw")));
        QVERIFY(vectorIndex.open(QIODevice::WriteOnly));
        vectorIndex.write("test");
        vectorIndex.close();

        QFile vectorMeta(QDir(dataDir).filePath(QStringLiteral("vectors-v2.meta")));
        QVERIFY(vectorMeta.open(QIODevice::WriteOnly));
        vectorMeta.write("test");
        vectorMeta.close();
    }

    // Parser/filter corpus.
    const QString pdfPath = QDir(docsDir).filePath(QStringLiteral("breaking-sound-barrier.pdf"));
    const QString txtPath = QDir(docsDir).filePath(QStringLiteral("breaking-sound-barrier.txt"));
    QVERIFY(upsertItem(store, pdfPath, QStringLiteral(".pdf"), bs::ItemKind::Pdf,
                       QStringLiteral("breaking sound barrier report")).has_value());
    QVERIFY(upsertItem(store, txtPath, QStringLiteral(".txt"), bs::ItemKind::Text,
                       QStringLiteral("breaking sound barrier report")).has_value());

    // Typo guardrail corpus.
    const QString bankingPath = QDir(docsDir).filePath(QStringLiteral("banking-report.txt"));
    QVERIFY(upsertItem(store, bankingPath, QStringLiteral(".txt"), bs::ItemKind::Text,
                       QStringLiteral("banking report report report report q1 summary")).has_value());
    const QString apiDeploymentPath = QDir(docsDir).filePath(QStringLiteral("API-deployment-guide.md"));
    QVERIFY(upsertItem(store, apiDeploymentPath, QStringLiteral(".md"), bs::ItemKind::Markdown,
                       QStringLiteral("API deployment guide for release operations")).has_value());
    const QString baselineClipboardPath = QDir(docsDir).filePath(QStringLiteral("meeting-rollout-notes.md"));
    QVERIFY(upsertItem(store, baselineClipboardPath, QStringLiteral(".md"), bs::ItemKind::Markdown,
                       QStringLiteral("rollout checklist reliability agenda")).has_value());
    const QString targetedClipboardPath = QDir(docsDir).filePath(QStringLiteral("deep-dive-notes.md"));
    QVERIFY(upsertItem(store, targetedClipboardPath, QStringLiteral(".md"), bs::ItemKind::Markdown,
                       QStringLiteral("rollout checklist reliability agenda")).has_value());

    // Placeholder/offline corpus.
    const QString creditPath = QDir(docsDir).filePath(QStringLiteral("credit report.pdf"));
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    auto creditId = store.upsertItem(
        creditPath,
        QStringLiteral("credit report.pdf"),
        QStringLiteral(".pdf"),
        bs::ItemKind::Pdf,
        1024,
        now,
        now,
        QString(),
        QStringLiteral("normal"),
        docsDir);
    QVERIFY(creditId.has_value());
    QVERIFY(store.recordFailure(
        creditId.value(),
        QStringLiteral("extraction"),
        QStringLiteral("File appears to be a cloud placeholder (size reported but no content readable)")));

    const QString queryBinary = findQueryBinary();
    QVERIFY2(!queryBinary.isEmpty(), "Could not locate betterspotlight-query binary");

    const QString querySocket =
        QDir(tempSocketDir.path()).filePath(QStringLiteral("query.sock"));
    const QString indexerSocket =
        QDir(tempSocketDir.path()).filePath(QStringLiteral("indexer.sock"));
    QFile::remove(querySocket);
    QFile::remove(indexerSocket);

    QProcess queryProcess;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    sanitizeChildEnvironment(env);
    env.insert(QStringLiteral("HOME"), tempHome.path());
    env.insert(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR"), dataDir);
    env.insert(QStringLiteral("BETTERSPOTLIGHT_SOCKET_DIR"), tempSocketDir.path());
    env.insert(QStringLiteral("BETTERSPOTLIGHT_PIPELINE_ACTOR_MODE"), QStringLiteral("legacy"));
    env.insert(QStringLiteral("BETTERSPOTLIGHT_HEALTH_SOURCE_MODE"), QStringLiteral("legacy"));
    env.insert(QStringLiteral("BETTERSPOTLIGHT_CONTROL_PLANE_MODE"), QStringLiteral("legacy"));
    env.remove(QStringLiteral("BETTERSPOTLIGHT_ALLOW_UNSUPPORTED_RUNTIME_MODES"));
    queryProcess.setProcessEnvironment(env);
    queryProcess.setProgram(queryBinary);
    queryProcess.setArguments({});
    queryProcess.setProcessChannelMode(QProcess::ForwardedChannels);
    queryProcess.start();
    QVERIFY2(queryProcess.waitForStarted(5000), "Failed to start query service process");
    auto processGuard = qScopeGuard([&]() {
        if (queryProcess.state() != QProcess::NotRunning) {
            queryProcess.kill();
            queryProcess.waitForFinished(2000);
        }
    });

    bs::SocketClient queryClient;
    QVERIFY2(waitForQueryConnection(queryClient, querySocket, 5000),
             qPrintable(QStringLiteral("Failed to connect to query service socket: %1").arg(querySocket)));

    // Queue source should be unavailable when no indexer responds.
    {
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("getHealth"));
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject indexHealth = response.value(QStringLiteral("result"))
                                            .toObject()
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        QCOMPARE(indexHealth.value(QStringLiteral("queueSource")).toString(),
                 QStringLiteral("unavailable"));
        QVERIFY(indexHealth.contains(QStringLiteral("queryHealthSnapshotState")));
        QVERIFY(indexHealth.contains(QStringLiteral("queryHealthSnapshotTimeMs")));
        QVERIFY(indexHealth.contains(QStringLiteral("queryHealthSnapshotLagMs")));
        const QString healthStatusReason =
            indexHealth.value(QStringLiteral("healthStatusReason")).toString();
        QVERIFY(healthStatusReason == QStringLiteral("indexer_unavailable")
                || healthStatusReason == QStringLiteral("health_snapshot_refreshing")
                || healthStatusReason == QStringLiteral("health_snapshot_cold_start"));
        QCOMPARE(indexHealth.value(QStringLiteral("criticalFailures")).toInt(), 0);
        const int expectedGapFailures = indexHealth.value(QStringLiteral("expectedGapFailures")).toInt();
        const QString snapshotState =
            indexHealth.value(QStringLiteral("queryHealthSnapshotState")).toString();
        if (snapshotState == QStringLiteral("fresh")) {
            QCOMPARE(expectedGapFailures, 1);
        } else {
            QVERIFY(expectedGapFailures >= 0 && expectedGapFailures <= 1);
        }
        if (indexHealth.contains(QStringLiteral("requiredModelInventoryReady"))) {
            QVERIFY(indexHealth.value(QStringLiteral("requiredModelInventoryReady")).isBool());
        }
        if (indexHealth.contains(QStringLiteral("requiredModelInventoryReason"))) {
            const QString requiredInventoryReason =
                indexHealth.value(QStringLiteral("requiredModelInventoryReason")).toString();
            QVERIFY(requiredInventoryReason == QStringLiteral("ready")
                    || requiredInventoryReason == QStringLiteral("required_models_unavailable"));
        }
        if (indexHealth.contains(QStringLiteral("vectorMigrationRequired"))) {
            QVERIFY(indexHealth.value(QStringLiteral("vectorMigrationRequired")).isBool());
        }
        if (indexHealth.contains(QStringLiteral("vectorGenerationState"))) {
            const QString vectorGenerationState =
                indexHealth.value(QStringLiteral("vectorGenerationState")).toString();
            QVERIFY(vectorGenerationState == QStringLiteral("ready")
                    || vectorGenerationState == QStringLiteral("migration_required"));
        }
        if (indexHealth.contains(QStringLiteral("vectorMigrationReason"))) {
            const QString vectorMigrationReason =
                indexHealth.value(QStringLiteral("vectorMigrationReason")).toString();
            QVERIFY(vectorMigrationReason.isEmpty()
                    || vectorMigrationReason == QStringLiteral("target_generation_not_active"));
        }
        if (indexHealth.contains(QStringLiteral("vectorGenerationActive"))) {
            QVERIFY(!indexHealth.value(QStringLiteral("vectorGenerationActive")).toString().isEmpty());
        }
        if (indexHealth.contains(QStringLiteral("vectorGenerationTarget"))) {
            QVERIFY(!indexHealth.value(QStringLiteral("vectorGenerationTarget")).toString().isEmpty());
        }
        if (indexHealth.contains(QStringLiteral("vectorGenerationSource"))) {
            const QString vectorGenerationSource =
                indexHealth.value(QStringLiteral("vectorGenerationSource")).toString();
            QVERIFY(vectorGenerationSource == QStringLiteral("settings")
                    || vectorGenerationSource == QStringLiteral("vector_generation_state")
                    || vectorGenerationSource == QStringLiteral("runtime_fallback"));
        }
        if (indexHealth.contains(QStringLiteral("vectorGenerationConsistency"))) {
            const QString vectorGenerationConsistency =
                indexHealth.value(QStringLiteral("vectorGenerationConsistency")).toString();
            QVERIFY(vectorGenerationConsistency == QStringLiteral("consistent")
                    || vectorGenerationConsistency == QStringLiteral("settings_vs_state_mismatch")
                    || vectorGenerationConsistency == QStringLiteral("state_vs_files_mismatch"));
        }
        if (indexHealth.contains(QStringLiteral("runtimeSettings"))) {
            const QJsonObject runtimeSettings =
                indexHealth.value(QStringLiteral("runtimeSettings")).toObject();
            QCOMPARE(runtimeSettings.value(QStringLiteral("pipelineActorModeEffective")).toString(),
                     QStringLiteral("actor_primary"));
            QCOMPARE(runtimeSettings.value(QStringLiteral("healthSourceModeEffective")).toString(),
                     QStringLiteral("aggregator_primary"));
            QCOMPARE(runtimeSettings.value(QStringLiteral("controlPlaneModeEffective")).toString(),
                     QStringLiteral("actor_primary"));
            QVERIFY(runtimeSettings.value(QStringLiteral("pipelineActorModeCoerced")).toBool(false));
            QVERIFY(runtimeSettings.value(QStringLiteral("healthSourceModeCoerced")).toBool(false));
            QVERIFY(runtimeSettings.value(QStringLiteral("controlPlaneModeCoerced")).toBool(false));
            QVERIFY(!runtimeSettings.value(QStringLiteral("unsupportedRuntimeModesAllowed")).toBool(true));
            assertFiniteDoubleValue(runtimeSettings,
                                    QStringLiteral("queryRouterMinConfidence"),
                                    0.45);
            assertFiniteDoubleValue(runtimeSettings, QStringLiteral("bm25WeightName"), 10.0);
            assertFiniteDoubleValue(runtimeSettings, QStringLiteral("bm25WeightPath"), 5.0);
            assertFiniteDoubleValue(runtimeSettings, QStringLiteral("bm25WeightContent"), 1.0);
        }
        QCOMPARE(store.getSetting(QStringLiteral("activeVectorGeneration")).value_or(QString()),
                 QStringLiteral("v2"));
        if (indexHealth.contains(QStringLiteral("m2ModulesInitialized"))) {
            QVERIFY(indexHealth.value(QStringLiteral("m2ModulesInitialized")).isBool());
        }
    }

    // Start fake indexer and verify queue parity fields.
    bs::test::ThreadedSocketServer fakeIndexer;
    QVERIFY2(fakeIndexer.start(indexerSocket, [&tempHome](const QJsonObject& request) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        if (method == QLatin1String("getQueueStatus")) {
            QJsonObject result;
            result[QStringLiteral("pending")] = 4200;
            result[QStringLiteral("processing")] = 2;
            result[QStringLiteral("failed")] = 0;
            result[QStringLiteral("dropped")] = 7;
            result[QStringLiteral("paused")] = false;
            result[QStringLiteral("preparing")] = 2;
            result[QStringLiteral("writing")] = 0;
            result[QStringLiteral("coalesced")] = 11;
            result[QStringLiteral("staleDropped")] = 3;
            result[QStringLiteral("prepWorkers")] = 3;
            result[QStringLiteral("writerBatchDepth")] = 1;
            QJsonArray roots;
            roots.append(tempHome.path());
            result[QStringLiteral("roots")] = roots;
            return bs::IpcMessage::makeResponse(id, result);
        }
        if (method == QLatin1String("ping")) {
            QJsonObject result;
            result[QStringLiteral("pong")] = true;
            return bs::IpcMessage::makeResponse(id, result);
        }
        return bs::IpcMessage::makeError(
            id, bs::IpcErrorCode::NotFound, QStringLiteral("Unsupported method"));
    }), "Failed to start fake indexer socket server");

    {
        QJsonObject response;
        QJsonObject indexHealth;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            response = sendOrFail(queryClient, QStringLiteral("getHealth"));
            if (response.value(QStringLiteral("type")).toString() == QLatin1String("response")) {
                indexHealth = response.value(QStringLiteral("result"))
                                  .toObject()
                                  .value(QStringLiteral("indexHealth"))
                                  .toObject();
                if (indexHealth.value(QStringLiteral("queueSource")).toString()
                        == QLatin1String("indexer_rpc")
                    && indexHealth.value(QStringLiteral("queryHealthSnapshotState")).toString()
                        == QLatin1String("fresh")) {
                    break;
                }
            }
            QTest::qWait(50);
        }
        QCOMPARE(indexHealth.value(QStringLiteral("queueSource")).toString(),
                 QStringLiteral("indexer_rpc"));
        const QString queueSnapshotState =
            indexHealth.value(QStringLiteral("queryHealthSnapshotState")).toString();
        QVERIFY(queueSnapshotState == QStringLiteral("fresh")
                || queueSnapshotState == QStringLiteral("refreshing"));
        const QString peerProbeState = indexHealth.value(QStringLiteral("peerProbeStateByService"))
                                           .toObject()
                                           .value(QStringLiteral("indexer"))
                                           .toString();
        QVERIFY(peerProbeState == QStringLiteral("fresh")
                || peerProbeState == QStringLiteral("refreshing"));
        const QString queueHealthReason =
            indexHealth.value(QStringLiteral("healthStatusReason")).toString();
        QVERIFY(queueHealthReason == QStringLiteral("healthy")
                || queueHealthReason == QStringLiteral("health_snapshot_refreshing"));
        QCOMPARE(indexHealth.value(QStringLiteral("criticalFailures")).toInt(), 0);
        const int queueExpectedGapFailures =
            indexHealth.value(QStringLiteral("expectedGapFailures")).toInt();
        if (queueSnapshotState == QStringLiteral("fresh")) {
            QCOMPARE(queueExpectedGapFailures, 1);
        } else {
            QVERIFY(queueExpectedGapFailures >= 0 && queueExpectedGapFailures <= 1);
        }
        QCOMPARE(indexHealth.value(QStringLiteral("queuePending")).toInt(), 4200);
        QCOMPARE(indexHealth.value(QStringLiteral("queueInProgress")).toInt(), 2);
        QCOMPARE(indexHealth.value(QStringLiteral("queuePreparing")).toInt(), 2);
        QCOMPARE(indexHealth.value(QStringLiteral("queueCoalesced")).toInt(), 11);
        QVERIFY(!indexHealth.value(QStringLiteral("vectorMigrationRequired")).toBool(true));
        QCOMPARE(indexHealth.value(QStringLiteral("vectorGenerationState")).toString(),
                 QStringLiteral("ready"));
        QCOMPARE(indexHealth.value(QStringLiteral("vectorGenerationActive")).toString(),
                 QStringLiteral("v2"));
        QCOMPARE(indexHealth.value(QStringLiteral("vectorGenerationSource")).toString(),
                 QStringLiteral("settings"));
        QVERIFY(indexHealth.value(QStringLiteral("retrievalAdvisory")).toObject().contains(
            QStringLiteral("code")));
    }

    // Health details endpoint should expose paginated failures + process/query stats.
    {
        QJsonObject params;
        params[QStringLiteral("limit")] = 25;
        params[QStringLiteral("offset")] = 0;
        QJsonObject response;
        QJsonObject result;
        QJsonObject details;
        QElapsedTimer detailsTimer;
        detailsTimer.start();
        while (detailsTimer.elapsed() < 3000) {
            response = sendOrFail(queryClient, QStringLiteral("getHealthDetails"), params);
            QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
            result = response.value(QStringLiteral("result")).toObject();
            details = result.value(QStringLiteral("details")).toObject();
            if (details.value(QStringLiteral("detailsState")).toString() == QLatin1String("fresh")
                && !details.value(QStringLiteral("failures")).toArray().isEmpty()) {
                break;
            }
            QTest::qWait(50);
        }
        QVERIFY(!details.isEmpty());

        const QJsonArray failures = details.value(QStringLiteral("failures")).toArray();
        const QString detailsState =
            details.value(QStringLiteral("detailsState")).toString();
        if (failures.isEmpty()) {
            QVERIFY(detailsState == QStringLiteral("stale")
                    || detailsState == QStringLiteral("refreshing")
                    || detailsState == QStringLiteral("unavailable"));
        } else {
            bool foundExpectedGap = false;
            for (const QJsonValue& value : failures) {
                const QJsonObject entry = value.toObject();
                if (entry.value(QStringLiteral("expectedGap")).toBool(false)) {
                    foundExpectedGap = true;
                    break;
                }
            }
            QVERIFY(foundExpectedGap);
        }

        const QJsonObject processStats = details.value(QStringLiteral("processStats")).toObject();
        QVERIFY(processStats.contains(QStringLiteral("query")));
        const QJsonObject queryStats = processStats.value(QStringLiteral("query")).toObject();
        QVERIFY(queryStats.contains(QStringLiteral("available")));
        QVERIFY(queryStats.value(QStringLiteral("available")).isBool());

        QVERIFY(details.contains(QStringLiteral("queryStats")));
        QVERIFY(details.contains(QStringLiteral("bsignore")));
        QVERIFY(details.contains(QStringLiteral("detailsState")));
        QVERIFY(details.contains(QStringLiteral("detailsTimeMs")));
        QVERIFY(details.contains(QStringLiteral("detailsLagMs")));
    }

    // Parser wiring + filter merge behavior.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("breaking sound barrier pdf");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        QJsonObject response;
        QElapsedTimer searchTimer;
        searchTimer.start();
        while (searchTimer.elapsed() < 20000) {
            response = sendOrFail(queryClient, QStringLiteral("search"), params, 8000);
            if (response.value(QStringLiteral("type")).toString() == QLatin1String("response")) {
                break;
            }
            QTest::qWait(100);
        }
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("queryAfterParse")).toString(),
                 QStringLiteral("breaking sound barrier"));
        const QJsonArray parsedTypes = debugInfo.value(QStringLiteral("parsedTypes")).toArray();
        QVERIFY(!parsedTypes.isEmpty());
        QCOMPARE(parsedTypes.first().toString(), QStringLiteral("pdf"));
        QVERIFY(debugInfo.value(QStringLiteral("plannerApplied")).toBool(false));
        QCOMPARE(debugInfo.value(QStringLiteral("plannerReason")).toString(),
                 QStringLiteral("consumer_curated_prefilter"));
        QCOMPARE(debugInfo.value(QStringLiteral("queryClass")).toString(),
                 QStringLiteral("natural_language"));
        assertFiniteDoubleValue(debugInfo, QStringLiteral("queryRouterMinConfidence"), 0.45);
        assertFiniteDoubleValue(debugInfo, QStringLiteral("onlineRankerBlendAlpha"), 0.15);
        const double lexicalWeight =
            debugInfo.value(QStringLiteral("mergeLexicalWeightApplied")).toDouble();
        const double semanticWeight =
            debugInfo.value(QStringLiteral("mergeSemanticWeightApplied")).toDouble();
        const bool adaptiveApplied =
            debugInfo.value(QStringLiteral("adaptiveMergeWeightsApplied")).toBool(false);
        if (adaptiveApplied) {
            QVERIFY(std::abs(lexicalWeight - 0.45) < 1e-6);
            QVERIFY(std::abs(semanticWeight - 0.55) < 1e-6);
        } else {
            QVERIFY(std::abs(lexicalWeight - 0.55) < 1e-6);
            QVERIFY(std::abs(semanticWeight - 0.45) < 1e-6);
        }
        QVERIFY(std::abs((lexicalWeight + semanticWeight) - 1.0) < 1e-6);
        QVERIFY(debugInfo.contains(QStringLiteral("semanticOnlySuppressedCount")));
        QVERIFY(debugInfo.contains(QStringLiteral("semanticOnlyAdmittedCount")));
        QVERIFY(debugInfo.value(QStringLiteral("semanticOnlyAdmitReasonSummary")).isObject());
        const QJsonObject filtersDebug = debugInfo.value(QStringLiteral("filters")).toObject();
        const QJsonArray includePaths = filtersDebug.value(QStringLiteral("includePaths")).toArray();
        QVERIFY(!includePaths.isEmpty());

        const QJsonArray results = result.value(QStringLiteral("results")).toArray();
        QVERIFY(!results.isEmpty());
        for (const QJsonValue& value : results) {
            const QString name = value.toObject().value(QStringLiteral("name")).toString().toLower();
            QVERIFY2(name.endsWith(QStringLiteral(".pdf")),
                     qPrintable(QStringLiteral("Unexpected non-pdf result: %1").arg(name)));
        }

        const QJsonObject postSearchHealth = sendOrFail(queryClient, QStringLiteral("getHealth"));
        QCOMPARE(postSearchHealth.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject postSearchIndexHealth = postSearchHealth.value(QStringLiteral("result"))
                                                      .toObject()
                                                      .value(QStringLiteral("indexHealth"))
                                                      .toObject();
        QVERIFY(postSearchIndexHealth.value(QStringLiteral("m2ModulesInitialized")).isBool());
    }

    // User-triggered answer snippet preview should run off the ranking path.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("breaking sound barrier");
        params[QStringLiteral("path")] = pdfPath;
        params[QStringLiteral("timeoutMs")] = 500;
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("getAnswerSnippet"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        QVERIFY(result.value(QStringLiteral("available")).toBool(false));
        const QString answer = result.value(QStringLiteral("answer")).toString().toLower();
        QVERIFY(answer.contains(QStringLiteral("breaking")));
        QVERIFY(answer.contains(QStringLiteral("barrier")));
        const QString source = result.value(QStringLiteral("source")).toString();
        QVERIFY(source == QStringLiteral("extractive_preview")
                || source == QStringLiteral("qa_extractive_model"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("does not exist");
        params[QStringLiteral("path")] = QStringLiteral("/tmp/not-found.txt");
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("getAnswerSnippet"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        QVERIFY(!result.value(QStringLiteral("available")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("item_not_found"));
    }

    // Typo guardrail checks.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("banikng report");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("strict");
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("search"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject debugInfo = response.value(QStringLiteral("result"))
                                          .toObject()
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("queryMode")).toString(), QStringLiteral("strict"));
        QVERIFY(!debugInfo.value(QStringLiteral("rewriteApplied")).toBool(true));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("banikng");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("auto");
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("search"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject debugInfo = response.value(QStringLiteral("result"))
                                          .toObject()
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QVERIFY(debugInfo.contains(QStringLiteral("rewriteApplied")));
        QVERIFY(debugInfo.contains(QStringLiteral("rewriteReason")));
        QVERIFY(debugInfo.value(QStringLiteral("rewriteApplied")).toBool());
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("banikng repotr");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("auto");
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("search"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject debugInfo = response.value(QStringLiteral("result"))
                                          .toObject()
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QVERIFY(debugInfo.value(QStringLiteral("rewriteApplied")).toBool());
        const QJsonArray correctedTokens = debugInfo.value(QStringLiteral("correctedTokens")).toArray();
        QVERIFY2(correctedTokens.size() <= 2, "Auto-mode rewrite exceeded replacement budget");
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("API deplyoment guide");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("auto");
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("search"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(debugInfo.value(QStringLiteral("rewriteApplied")).toBool());
        const QJsonArray ranked = result.value(QStringLiteral("results")).toArray();
        QVERIFY(!ranked.isEmpty());
        const QString topName = ranked.first().toObject().value(QStringLiteral("name")).toString();
        QCOMPARE(topName, QStringLiteral("API-deployment-guide.md"));
    }

    // Clipboard context signals should boost matching path hints without storing raw clipboard text.
    {
        QJsonObject baseParams;
        baseParams[QStringLiteral("query")] = QStringLiteral("rollout checklist");
        baseParams[QStringLiteral("limit")] = 10;
        baseParams[QStringLiteral("debug")] = true;
        const QJsonObject baseResponse =
            sendOrFail(queryClient, QStringLiteral("search"), baseParams, 8000);
        QCOMPARE(baseResponse.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonArray baseResults = baseResponse.value(QStringLiteral("result"))
                                     .toObject()
                                     .value(QStringLiteral("results"))
                                     .toArray();
        QVERIFY(baseResults.size() >= 2);
        const QString baseTopPath = baseResults.first().toObject().value(QStringLiteral("path")).toString();
        QCOMPARE(baseTopPath, baselineClipboardPath);

        QJsonObject context;
        context[QStringLiteral("clipboardBasename")] = QStringLiteral("deep-dive-notes.md");
        context[QStringLiteral("clipboardDirname")] = QStringLiteral("documents");
        context[QStringLiteral("clipboardExtension")] = QStringLiteral("md");
        baseParams[QStringLiteral("context")] = context;
        const QJsonObject boostedResponse =
            sendOrFail(queryClient, QStringLiteral("search"), baseParams, 8000);
        QCOMPARE(boostedResponse.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject boostedResult = boostedResponse.value(QStringLiteral("result")).toObject();
        const QJsonArray boostedResults = boostedResult.value(QStringLiteral("results")).toArray();
        QVERIFY(boostedResults.size() >= 2);
        const QString boostedTopPath = boostedResults.first().toObject().value(QStringLiteral("path")).toString();
        QCOMPARE(boostedTopPath, targetedClipboardPath);

        const QJsonObject debugInfo = boostedResult.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(debugInfo.value(QStringLiteral("clipboardSignalsProvided")).toBool(false));
        QVERIFY(debugInfo.value(QStringLiteral("clipboardSignalBoostedResults")).toInt() > 0);
    }

    // Availability metadata for offline placeholder result.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("credit report");
        params[QStringLiteral("limit")] = 10;
        const QJsonObject response =
            sendOrFail(queryClient, QStringLiteral("search"), params, 8000);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonArray results = response.value(QStringLiteral("result"))
                                       .toObject()
                                       .value(QStringLiteral("results"))
                                       .toArray();
        bool foundOffline = false;
        for (const QJsonValue& value : results) {
            const QJsonObject obj = value.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString().toLower();
            if (name.contains(QStringLiteral("credit report"))) {
                QCOMPARE(obj.value(QStringLiteral("availabilityStatus")).toString(),
                         QStringLiteral("offline_placeholder"));
                QVERIFY(!obj.value(QStringLiteral("contentAvailable")).toBool(true));
                foundOffline = true;
                break;
            }
        }
        QVERIFY(foundOffline);
    }

    fakeIndexer.close();

    // Graceful shutdown.
    queryClient.sendRequest(QStringLiteral("shutdown"), {}, 1000);
    queryProcess.waitForFinished(5000);
    if (queryProcess.state() != QProcess::NotRunning) {
        queryProcess.kill();
        queryProcess.waitForFinished(2000);
    }
    processGuard.dismiss();
}

QTEST_MAIN(TestQueryServiceCoreImprovements)
#include "test_query_service_core_improvements.moc"
