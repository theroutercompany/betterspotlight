#include <QtTest/QtTest>

#include "core/shared/ipc_messages.h"
#include "ipc_test_utils.h"
#include "service_process_harness.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

class TestInferenceServiceIpc : public QObject {
    Q_OBJECT

private slots:
    void testInferenceIpcContract();
    void testHealthRequestNotBlockedByDeferredInferenceWork();
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

QTEST_MAIN(TestInferenceServiceIpc)
#include "test_inference_service_ipc.moc"
