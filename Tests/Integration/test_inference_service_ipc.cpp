#include <QtTest/QtTest>

#include "core/shared/ipc_messages.h"
#include "ipc_test_utils.h"
#include "service_process_harness.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

class TestInferenceServiceIpc : public QObject {
    Q_OBJECT

private slots:
    void testInferenceIpcContract();
    void testHealthRequestNotBlockedByDeferredInferenceWork();
    void testExpiredDeadlineBypassesBusyWorker();
    void testDeferredRequestCancellationSignalsHealth();
    void testRebuildAdmissionRespectsLiveInFlightPressure();
    void testHealthSnapshotsStayCoherentDuringStateChurn();
};

void TestInferenceServiceIpc::testInferenceIpcContract()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BETTERSPOTLIGHT_INFERENCE_SUPERVISOR_MODE"),
                      QStringLiteral("legacy"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    launch.startTimeoutMs = 20000;
    launch.connectTimeoutMs = 30000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 7000;
    QVERIFY2(harness.start(launch), "Failed to start inference service");

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("hello world");
        const QJsonObject response = harness.request(QStringLiteral("embed_query"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject payload = bs::test::resultPayload(response);
        QVERIFY(payload.contains(QStringLiteral("status")));
        QVERIFY(payload.contains(QStringLiteral("modelRole")));
    }

    {
        QJsonObject params;
        QJsonArray texts;
        texts.append(QStringLiteral("alpha"));
        texts.append(QStringLiteral("beta"));
        params[QStringLiteral("texts")] = texts;
        params[QStringLiteral("role")] = QStringLiteral("bi-encoder-fast");
        const QJsonObject response = harness.request(QStringLiteral("embed_passages"), params, 8000);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).contains(QStringLiteral("status")));
    }
    {
        QJsonObject params;
        QJsonArray texts;
        texts.append(QStringLiteral("gamma"));
        texts.append(QStringLiteral("delta"));
        params[QStringLiteral("texts")] = texts;
        params[QStringLiteral("role")] = QStringLiteral("bi-encoder-fast");
        params[QStringLiteral("priority")] = QStringLiteral("rebuild");
        params[QStringLiteral("microBatchSize")] = 1;
        const QJsonObject response = harness.request(QStringLiteral("embed_passages"), params, 8000);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).contains(QStringLiteral("status")));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("rank me");
        QJsonArray candidates;
        QJsonObject c1;
        c1[QStringLiteral("itemId")] = 1;
        c1[QStringLiteral("path")] = QStringLiteral("/tmp/a.txt");
        c1[QStringLiteral("name")] = QStringLiteral("a.txt");
        c1[QStringLiteral("snippet")] = QStringLiteral("rank me");
        c1[QStringLiteral("score")] = 1.0;
        candidates.append(c1);
        params[QStringLiteral("candidates")] = candidates;

        const QJsonObject fastResponse = harness.request(QStringLiteral("rerank_fast"), params, 5000);
        QVERIFY(bs::test::isResponse(fastResponse));
        QVERIFY(bs::test::resultPayload(fastResponse).contains(QStringLiteral("status")));

        const QJsonObject strongResponse = harness.request(QStringLiteral("rerank_strong"), params, 5000);
        QVERIFY(bs::test::isResponse(strongResponse));
        QVERIFY(bs::test::resultPayload(strongResponse).contains(QStringLiteral("status")));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("what is this?");
        QJsonArray contexts;
        contexts.append(QStringLiteral("This is a simple qa context."));
        params[QStringLiteral("contexts")] = contexts;
        const QJsonObject response = harness.request(QStringLiteral("qa_extract"), params, 6000);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).contains(QStringLiteral("status")));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("force timeout");
        params[QStringLiteral("deadlineMs")] = QDateTime::currentMSecsSinceEpoch() - 1;
        const QJsonObject response = harness.request(QStringLiteral("embed_query"), params, 3000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject payload = bs::test::resultPayload(response);
        QCOMPARE(payload.value(QStringLiteral("status")).toString(), QStringLiteral("timeout"));
        QVERIFY(!payload.value(QStringLiteral("fallbackReason")).toString().isEmpty());
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("cancel_request"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("cancelToken")] = QStringLiteral("cancel-me");
        const QJsonObject cancelResponse = harness.request(QStringLiteral("cancel_request"), params);
        QVERIFY(bs::test::isResponse(cancelResponse));
        QVERIFY(bs::test::resultPayload(cancelResponse).value(QStringLiteral("cancelled")).toBool(false));

        QJsonObject embedParams;
        embedParams[QStringLiteral("query")] = QStringLiteral("cancelled call");
        embedParams[QStringLiteral("requestId")] = QStringLiteral("cancelled-call-1");
        embedParams[QStringLiteral("cancelToken")] = QStringLiteral("cancel-me");
        const QJsonObject embedResponse =
            harness.request(QStringLiteral("embed_query"), embedParams, 5000);
        QVERIFY(bs::test::isResponse(embedResponse));
        QCOMPARE(bs::test::resultPayload(embedResponse).value(QStringLiteral("status")).toString(),
                 QStringLiteral("cancelled"));
    }

    for (int i = 0; i < 5; ++i) {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("restart-probe-%1").arg(i);
        params[QStringLiteral("role")] = QStringLiteral("bi-encoder");
        params[QStringLiteral("requestId")] = QStringLiteral("restart-probe-id-%1").arg(i);
        const QJsonObject response = harness.request(QStringLiteral("embed_query"), params, 4000);
        QVERIFY(bs::test::isResponse(response));
        const QString status = bs::test::resultPayload(response).value(QStringLiteral("status")).toString();
        QVERIFY(!status.isEmpty());
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("get_inference_health"));
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.contains(QStringLiteral("connected")));
        QVERIFY(result.contains(QStringLiteral("roleStatusByModel")));
        QVERIFY(result.contains(QStringLiteral("queueDepthByRole")));
        QVERIFY(result.contains(QStringLiteral("inFlightCountByRole")));
        QVERIFY(result.contains(QStringLiteral("inFlightByLaneByRole")));
        QVERIFY(result.contains(QStringLiteral("activeLaneByRole")));
        QVERIFY(result.contains(QStringLiteral("cancelledCountByRole")));
        QVERIFY(result.contains(QStringLiteral("timeoutCleanupCountByRole")));
        QVERIFY(result.contains(QStringLiteral("cancelSignalCount")));
        QVERIFY(result.contains(QStringLiteral("timeoutCleanupCount")));
        QVERIFY(result.contains(QStringLiteral("timeoutCountByRole")));
        QVERIFY(result.contains(QStringLiteral("failureCountByRole")));
        QVERIFY(result.contains(QStringLiteral("restartCountByRole")));
        QCOMPARE(result.value(QStringLiteral("requestedSupervisorMode")).toString(),
                 QStringLiteral("legacy"));
        QCOMPARE(result.value(QStringLiteral("effectiveSupervisorMode")).toString(),
                 QStringLiteral("actor_primary"));
        QVERIFY(result.value(QStringLiteral("supervisorModeCoerced")).toBool(false));
        QVERIFY(result.value(QStringLiteral("placeholderWorkersEnabled")).toBool(false));
        const QJsonObject queueDepthByRole = result.value(QStringLiteral("queueDepthByRole")).toObject();
        QVERIFY(queueDepthByRole.contains(QStringLiteral("bi-encoder")));
        QVERIFY(queueDepthByRole.contains(QStringLiteral("bi-encoder-rebuild")));
        const QJsonObject roleStatusByModel = result.value(QStringLiteral("roleStatusByModel")).toObject();
        QCOMPARE(roleStatusByModel.value(QStringLiteral("bi-encoder")).toString(),
                 QStringLiteral("test_placeholder"));
        const QJsonObject roleStateReasonByModel =
            result.value(QStringLiteral("roleStateReasonByModel")).toObject();
        QCOMPARE(roleStateReasonByModel.value(QStringLiteral("bi-encoder")).toString(),
                 QStringLiteral("placeholder_worker"));
        const QJsonObject restartCountByRole = result.value(QStringLiteral("restartCountByRole")).toObject();
        QVERIFY(restartCountByRole.contains(QStringLiteral("bi-encoder")));
    }
}

void TestInferenceServiceIpc::testHealthRequestNotBlockedByDeferredInferenceWork()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_REQUEST_DELAY_MS"), QStringLiteral("1200"));
    launch.startTimeoutMs = 20000;
    launch.connectTimeoutMs = 30000;
    launch.readyTimeoutMs = 30000;
    QVERIFY2(harness.start(launch), "Failed to start inference service");

    bs::SocketClient liveClient;
    QVERIFY2(bs::test::waitForSocketConnection(liveClient, harness.socketPath(), 5000),
             "Failed to connect live inference client");
    bs::SocketClient healthClient;
    QVERIFY2(bs::test::waitForSocketConnection(healthClient, harness.socketPath(), 5000),
             "Failed to connect health inference client");

    bool embedCompleted = false;
    std::optional<QJsonObject> embedResponse;
    QJsonObject params;
    params[QStringLiteral("query")] = QStringLiteral("long-running placeholder embed");
    liveClient.sendRequestAsync(
        QStringLiteral("embed_query"),
        params,
        4000,
        [&](const std::optional<QJsonObject>& response) {
            embedResponse = response;
            embedCompleted = true;
        });

    QTest::qWait(100);

    QElapsedTimer timer;
    timer.start();
    const QJsonObject healthResponse =
        bs::test::requestOrFailWithDiagnostics(healthClient,
                                               QStringLiteral("get_inference_health"),
                                               {},
                                               600,
                                               harness.socketPath());
    const qint64 elapsedMs = timer.elapsed();

    QVERIFY2(bs::test::isResponse(healthResponse),
             "Inference health request should succeed while embed request is still running");
    QVERIFY2(elapsedMs < 900,
             qPrintable(QStringLiteral("Health request took too long: %1ms").arg(elapsedMs)));

    QTRY_VERIFY_WITH_TIMEOUT(embedCompleted, 5000);
    QVERIFY(embedResponse.has_value());
    QVERIFY(bs::test::isResponse(embedResponse.value()));
}

void TestInferenceServiceIpc::testExpiredDeadlineBypassesBusyWorker()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_REQUEST_DELAY_MS"), QStringLiteral("1200"));
    launch.startTimeoutMs = 20000;
    launch.connectTimeoutMs = 30000;
    launch.readyTimeoutMs = 30000;
    QVERIFY2(harness.start(launch), "Failed to start inference service");

    bs::SocketClient liveClient;
    QVERIFY2(bs::test::waitForSocketConnection(liveClient, harness.socketPath(), 5000),
             "Failed to connect live inference client");
    bs::SocketClient deadlineClient;
    QVERIFY2(bs::test::waitForSocketConnection(deadlineClient, harness.socketPath(), 5000),
             "Failed to connect deadline inference client");

    bool liveCompleted = false;
    std::optional<QJsonObject> liveResponse;
    QJsonObject liveParams;
    liveParams[QStringLiteral("query")] = QStringLiteral("busy worker");
    liveParams[QStringLiteral("requestId")] = QStringLiteral("busy-worker-1");
    liveClient.sendRequestAsync(
        QStringLiteral("embed_query"),
        liveParams,
        5000,
        [&](const std::optional<QJsonObject>& response) {
            liveResponse = response;
            liveCompleted = true;
        });

    QTest::qWait(100);

    const QString cancelledToken = QStringLiteral("already-cancelled-while-busy");
    QJsonObject cancelParams;
    cancelParams[QStringLiteral("cancelToken")] = cancelledToken;
    const QJsonObject cancelResponse =
        bs::test::requestOrFailWithDiagnostics(deadlineClient,
                                               QStringLiteral("cancel_request"),
                                               cancelParams,
                                               700,
                                               harness.socketPath());
    QVERIFY(bs::test::isResponse(cancelResponse));

    QJsonObject cancelledParams;
    cancelledParams[QStringLiteral("query")] = QStringLiteral("cancelled before admission");
    cancelledParams[QStringLiteral("requestId")] = QStringLiteral("cancelled-while-busy-1");
    cancelledParams[QStringLiteral("cancelToken")] = cancelledToken;
    const QJsonObject cancelledResponse =
        bs::test::requestOrFailWithDiagnostics(deadlineClient,
                                               QStringLiteral("embed_query"),
                                               cancelledParams,
                                               700,
                                               harness.socketPath());
    QVERIFY2(bs::test::isResponse(cancelledResponse),
             "Already-cancelled request should receive an immediate cancellation response");
    const QJsonObject cancelledPayload = bs::test::resultPayload(cancelledResponse);
    QCOMPARE(cancelledPayload.value(QStringLiteral("status")).toString(),
             QStringLiteral("cancelled"));
    QCOMPARE(cancelledPayload.value(QStringLiteral("fallbackReason")).toString(),
             QStringLiteral("cancel_token"));

    QJsonObject expiredParams;
    expiredParams[QStringLiteral("query")] = QStringLiteral("already expired");
    expiredParams[QStringLiteral("requestId")] = QStringLiteral("expired-while-busy-1");
    expiredParams[QStringLiteral("deadlineMs")] = QDateTime::currentMSecsSinceEpoch() - 1;

    QElapsedTimer timer;
    timer.start();
    const QJsonObject expiredResponse =
        bs::test::requestOrFailWithDiagnostics(deadlineClient,
                                               QStringLiteral("embed_query"),
                                               expiredParams,
                                               700,
                                               harness.socketPath());
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(bs::test::isResponse(expiredResponse),
             "Expired request should receive an immediate timeout response");
    QVERIFY2(elapsedMs < 700,
             qPrintable(QStringLiteral("Expired request waited behind busy worker: %1ms")
                            .arg(elapsedMs)));
    const QJsonObject expiredPayload = bs::test::resultPayload(expiredResponse);
    QCOMPARE(expiredPayload.value(QStringLiteral("status")).toString(), QStringLiteral("timeout"));
    QCOMPARE(expiredPayload.value(QStringLiteral("fallbackReason")).toString(),
             QStringLiteral("deadline_exceeded"));

    QTRY_VERIFY_WITH_TIMEOUT(liveCompleted, 5000);
    QVERIFY(liveResponse.has_value());
    QVERIFY(bs::test::isResponse(liveResponse.value()));
}

void TestInferenceServiceIpc::testDeferredRequestCancellationSignalsHealth()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_REQUEST_DELAY_MS"), QStringLiteral("1200"));
    launch.startTimeoutMs = 20000;
    launch.connectTimeoutMs = 30000;
    launch.readyTimeoutMs = 30000;
    QVERIFY2(harness.start(launch), "Failed to start inference service");

    bs::SocketClient liveClient;
    QVERIFY2(bs::test::waitForSocketConnection(liveClient, harness.socketPath(), 5000),
             "Failed to connect live inference client");
    bs::SocketClient healthClient;
    QVERIFY2(bs::test::waitForSocketConnection(healthClient, harness.socketPath(), 5000),
             "Failed to connect health inference client");

    bool embedCompleted = false;
    std::optional<QJsonObject> embedResponse;
    const QString cancelToken = QStringLiteral("cancel-mid-flight");
    QJsonObject params;
    params[QStringLiteral("query")] = QStringLiteral("cancelled placeholder embed");
    params[QStringLiteral("requestId")] = QStringLiteral("cancelled-placeholder-1");
    params[QStringLiteral("cancelToken")] = cancelToken;
    liveClient.sendRequestAsync(
        QStringLiteral("embed_query"),
        params,
        5000,
        [&](const std::optional<QJsonObject>& response) {
            embedResponse = response;
            embedCompleted = true;
        });

    QTest::qWait(100);

    QJsonObject cancelParams;
    cancelParams[QStringLiteral("cancelToken")] = cancelToken;
    const QJsonObject cancelResponse =
        bs::test::requestOrFailWithDiagnostics(healthClient,
                                               QStringLiteral("cancel_request"),
                                               cancelParams,
                                               1000,
                                               harness.socketPath());
    QVERIFY(bs::test::isResponse(cancelResponse));
    QVERIFY(bs::test::resultPayload(cancelResponse).value(QStringLiteral("cancelled")).toBool(false));

    QTRY_VERIFY_WITH_TIMEOUT(embedCompleted, 5000);
    QVERIFY(embedResponse.has_value());
    QVERIFY(bs::test::isResponse(embedResponse.value()));
    const QJsonObject embedPayload = bs::test::resultPayload(embedResponse.value());
    QCOMPARE(embedPayload.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));

    const QJsonObject healthResponse =
        bs::test::requestOrFailWithDiagnostics(healthClient,
                                               QStringLiteral("get_inference_health"),
                                               {},
                                               1000,
                                               harness.socketPath());
    QVERIFY(bs::test::isResponse(healthResponse));
    const QJsonObject health = bs::test::resultPayload(healthResponse);
    QVERIFY(health.value(QStringLiteral("cancelSignalCount")).toInteger(0) >= 1);
    const QJsonObject cancelledCountByRole =
        health.value(QStringLiteral("cancelledCountByRole")).toObject();
    QVERIFY(cancelledCountByRole.value(QStringLiteral("bi-encoder")).toInteger(0) >= 1);
    const QJsonObject inFlightCountByRole =
        health.value(QStringLiteral("inFlightCountByRole")).toObject();
    QCOMPARE(inFlightCountByRole.value(QStringLiteral("bi-encoder")).toInteger(1), 0);
}

void TestInferenceServiceIpc::testRebuildAdmissionRespectsLiveInFlightPressure()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_REQUEST_DELAY_MS"), QStringLiteral("1200"));
    launch.startTimeoutMs = 20000;
    launch.connectTimeoutMs = 30000;
    launch.readyTimeoutMs = 30000;
    QVERIFY2(harness.start(launch), "Failed to start inference service");

    bs::SocketClient liveClient;
    QVERIFY2(bs::test::waitForSocketConnection(liveClient, harness.socketPath(), 5000),
             "Failed to connect live inference client");
    bs::SocketClient rebuildClient;
    QVERIFY2(bs::test::waitForSocketConnection(rebuildClient, harness.socketPath(), 5000),
             "Failed to connect rebuild inference client");

    bool liveCompleted = false;
    std::optional<QJsonObject> liveResponse;
    QJsonObject liveParams;
    liveParams[QStringLiteral("query")] = QStringLiteral("live-in-flight");
    liveParams[QStringLiteral("requestId")] = QStringLiteral("live-in-flight-1");
    liveClient.sendRequestAsync(
        QStringLiteral("embed_query"),
        liveParams,
        5000,
        [&](const std::optional<QJsonObject>& response) {
            liveResponse = response;
            liveCompleted = true;
        });

    QTest::qWait(100);

    QJsonObject rebuildParams;
    QJsonArray texts;
    texts.append(QStringLiteral("rebuild content"));
    rebuildParams[QStringLiteral("texts")] = texts;
    rebuildParams[QStringLiteral("priority")] = QStringLiteral("rebuild");
    rebuildParams[QStringLiteral("role")] = QStringLiteral("bi-encoder");
    rebuildParams[QStringLiteral("requestId")] = QStringLiteral("rebuild-while-live-1");
    const QJsonObject rebuildResponse =
        bs::test::requestOrFailWithDiagnostics(rebuildClient,
                                               QStringLiteral("embed_passages"),
                                               rebuildParams,
                                               1200,
                                               harness.socketPath());
    QVERIFY(bs::test::isResponse(rebuildResponse));
    const QJsonObject rebuildPayload = bs::test::resultPayload(rebuildResponse);
    QCOMPARE(rebuildPayload.value(QStringLiteral("status")).toString(), QStringLiteral("degraded"));
    QVERIFY(rebuildPayload.value(QStringLiteral("overload")).toBool(false));
    QCOMPARE(rebuildPayload.value(QStringLiteral("lane")).toString(), QStringLiteral("rebuild"));

    const QString fallbackReason = rebuildPayload.value(QStringLiteral("fallbackReason")).toString();
    QVERIFY(fallbackReason == QStringLiteral("worker_live_lane_busy")
            || fallbackReason == QStringLiteral("global_live_lane_priority"));

    QTRY_VERIFY_WITH_TIMEOUT(liveCompleted, 5000);
    QVERIFY(liveResponse.has_value());
    QVERIFY(bs::test::isResponse(liveResponse.value()));
}

void TestInferenceServiceIpc::testHealthSnapshotsStayCoherentDuringStateChurn()
{
    QTemporaryDir tempHome;
    QVERIFY(tempHome.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("inference"), QStringLiteral("betterspotlight-inference"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS"), QStringLiteral("1"));
    launch.env.insert(QStringLiteral("BS_TEST_INFERENCE_REQUEST_DELAY_MS"), QStringLiteral("40"));
    launch.startTimeoutMs = 20000;
    launch.connectTimeoutMs = 30000;
    launch.readyTimeoutMs = 30000;
    QVERIFY2(harness.start(launch), "Failed to start inference service");

    bs::SocketClient liveClient;
    QVERIFY2(bs::test::waitForSocketConnection(liveClient, harness.socketPath(), 5000),
             "Failed to connect live inference client");
    bs::SocketClient healthClient;
    QVERIFY2(bs::test::waitForSocketConnection(healthClient, harness.socketPath(), 5000),
             "Failed to connect health inference client");

    constexpr int kRequestCount = 24;
    int completed = 0;
    int failedCallbacks = 0;
    for (int i = 0; i < kRequestCount; ++i) {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("state-churn-%1").arg(i);
        params[QStringLiteral("requestId")] = QStringLiteral("state-churn-id-%1").arg(i);
        liveClient.sendRequestAsync(
            QStringLiteral("embed_query"),
            params,
            5000,
            [&](const std::optional<QJsonObject>& response) {
                if (!response.has_value() || !bs::test::isResponse(response.value())) {
                    ++failedCallbacks;
                }
                ++completed;
            });
    }

    int healthSnapshots = 0;
    QElapsedTimer timer;
    timer.start();
    while (completed < kRequestCount && timer.elapsed() < 7000) {
        const QJsonObject healthResponse =
            bs::test::requestOrFailWithDiagnostics(healthClient,
                                                   QStringLiteral("get_inference_health"),
                                                   {},
                                                   1000,
                                                   harness.socketPath());
        QVERIFY(bs::test::isResponse(healthResponse));
        const QJsonObject health = bs::test::resultPayload(healthResponse);
        const QJsonObject roleStatusByModel =
            health.value(QStringLiteral("roleStatusByModel")).toObject();
        const QJsonObject roleStateReasonByModel =
            health.value(QStringLiteral("roleStateReasonByModel")).toObject();
        const QJsonObject admissionByModel =
            health.value(QStringLiteral("roleAdmissionByModel")).toObject();

        QVERIFY(roleStatusByModel.contains(QStringLiteral("bi-encoder")));
        QVERIFY(roleStateReasonByModel.contains(QStringLiteral("bi-encoder")));
        QVERIFY(admissionByModel.contains(QStringLiteral("bi-encoder")));
        QVERIFY(!roleStatusByModel.value(QStringLiteral("bi-encoder")).toString().isEmpty());
        QVERIFY(!roleStateReasonByModel.value(QStringLiteral("bi-encoder")).toString().isEmpty());
        ++healthSnapshots;

        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }

    QTRY_COMPARE_WITH_TIMEOUT(completed, kRequestCount, 7000);
    QCOMPARE(failedCallbacks, 0);
    QVERIFY2(healthSnapshots >= 3,
             qPrintable(QStringLiteral("Expected at least 3 concurrent health snapshots, got %1")
                            .arg(healthSnapshots)));
}

QTEST_MAIN(TestInferenceServiceIpc)
#include "test_inference_service_ipc.moc"
