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

#include <optional>
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

} // namespace

class TestQueryServiceM2Ipc : public QObject {
    Q_OBJECT

private slots:
    void testQueryM2IpcContract();
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
        QVERIFY(result.value(QStringLiteral("interactions")).isArray());
        QVERIFY(result.value(QStringLiteral("count")).toInt() >= 1);
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_learning_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("learning")).isObject());
    }

    {
        QJsonObject params;
        params[QStringLiteral("behaviorStreamEnabled")] = true;
        params[QStringLiteral("learningEnabled")] = true;
        params[QStringLiteral("learningPauseOnUserInput")] = true;
        QJsonArray denylist;
        denylist.append(QStringLiteral("com.example.secret"));
        params[QStringLiteral("denylistApps")] = denylist;

        const QJsonObject response = harness.request(QStringLiteral("set_learning_consent"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("updated")).toBool(false));
        QVERIFY(result.value(QStringLiteral("learning")).isObject());
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
        QVERIFY(result.value(QStringLiteral("learningHealth")).isObject());
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("trigger_learning_cycle"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.contains(QStringLiteral("promoted")));
        QVERIFY(result.contains(QStringLiteral("reason")));
        QVERIFY(result.value(QStringLiteral("learning")).isObject());
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

QTEST_MAIN(TestQueryServiceM2Ipc)
#include "test_query_service_m2_ipc.moc"
