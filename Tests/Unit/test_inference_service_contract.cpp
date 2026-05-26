#include <QtTest/QtTest>

#include "core/ipc/message.h"
#include "relevance_harness.h"
#include "services/inference/inference_service.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* key, const QByteArray& value)
        : m_key(key)
        , m_hadOriginal(qEnvironmentVariableIsSet(key))
        , m_original(qgetenv(key))
    {
        qputenv(m_key, value);
    }

    ~ScopedEnvVar()
    {
        if (m_hadOriginal) {
            qputenv(m_key, m_original);
        } else {
            qunsetenv(m_key);
        }
    }

private:
    const char* m_key;
    bool m_hadOriginal = false;
    QByteArray m_original;
};

QJsonObject request(uint64_t id, const QString& method, const QJsonObject& params = {})
{
    return bs::IpcMessage::makeRequest(id, method, params);
}

QJsonObject resultPayload(const QJsonObject& response)
{
    if (response.value(QStringLiteral("type")).toString() != QLatin1String("response")) {
        QTest::qFail("Expected IPC response envelope", __FILE__, __LINE__);
        return {};
    }
    return response.value(QStringLiteral("result")).toObject();
}

void expectPlaceholderStatus(const QJsonObject& payload, const QString& role)
{
    QCOMPARE(payload.value(QStringLiteral("status")).toString(), QStringLiteral("degraded"));
    QCOMPARE(payload.value(QStringLiteral("fallbackReason")).toString(),
             QStringLiteral("placeholder_worker"));
    QCOMPARE(payload.value(QStringLiteral("modelRole")).toString(), role);
}

} // namespace

class TestInferenceServiceContract : public QObject {
    Q_OBJECT

private slots:
    void testSynchronousDispatchRoutesAllRoles();
    void testSynchronousTimeoutPublishesCleanupHealth();
    void testAvailableWorkersWarmBeforeReportingReady();
};

void TestInferenceServiceContract::testSynchronousDispatchRoutesAllRoles()
{
    ScopedEnvVar deterministic("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP", QByteArrayLiteral("1"));
    ScopedEnvVar placeholders("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS", QByteArrayLiteral("1"));
    ScopedEnvVar delay("BS_TEST_INFERENCE_REQUEST_DELAY_MS", QByteArrayLiteral("0"));

    bs::InferenceService service;

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("systems ranking");
        const QJsonObject payload =
            resultPayload(service.dispatchForTest(request(1, QStringLiteral("embed_query"), params)));
        expectPlaceholderStatus(payload, QStringLiteral("bi-encoder"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("fast ranking");
        params[QStringLiteral("role")] = QStringLiteral("bi-encoder-fast");
        const QJsonObject payload =
            resultPayload(service.dispatchForTest(request(2, QStringLiteral("embed_query"), params)));
        expectPlaceholderStatus(payload, QStringLiteral("bi-encoder-fast"));
    }

    {
        QJsonArray texts;
        texts.append(QStringLiteral("alpha"));
        texts.append(QStringLiteral("beta"));
        QJsonObject params;
        params[QStringLiteral("texts")] = texts;
        params[QStringLiteral("role")] = QStringLiteral("bi-encoder-fast");
        params[QStringLiteral("priority")] = QStringLiteral("rebuild");
        const QJsonObject payload =
            resultPayload(service.dispatchForTest(request(3, QStringLiteral("embed_passages"), params)));
        expectPlaceholderStatus(payload, QStringLiteral("bi-encoder-fast-rebuild"));
    }

    {
        QJsonObject candidate;
        candidate[QStringLiteral("itemId")] = 1;
        candidate[QStringLiteral("score")] = 0.25;
        QJsonArray candidates;
        candidates.append(candidate);

        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("rerank");
        params[QStringLiteral("candidates")] = candidates;

        const QJsonObject fast =
            resultPayload(service.dispatchForTest(request(4, QStringLiteral("rerank_fast"), params)));
        expectPlaceholderStatus(fast, QStringLiteral("cross-encoder-fast"));

        const QJsonObject strong =
            resultPayload(service.dispatchForTest(request(5, QStringLiteral("rerank_strong"), params)));
        expectPlaceholderStatus(strong, QStringLiteral("cross-encoder"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("what");
        params[QStringLiteral("contexts")] = QJsonArray{QStringLiteral("context")};
        const QJsonObject payload =
            resultPayload(service.dispatchForTest(request(6, QStringLiteral("qa_extract"), params)));
        expectPlaceholderStatus(payload, QStringLiteral("qa-extractive"));
    }

    const QJsonObject health =
        resultPayload(service.dispatchForTest(request(7, QStringLiteral("get_inference_health"))));
    QVERIFY(health.value(QStringLiteral("placeholderWorkersEnabled")).toBool(false));
    QCOMPARE(health.value(QStringLiteral("requestExecutionMode")).toString(),
             QStringLiteral("deferred_async"));
    const QJsonObject statusByRole =
        health.value(QStringLiteral("roleStatusByModel")).toObject();
    QCOMPARE(statusByRole.value(QStringLiteral("bi-encoder")).toString(),
             QStringLiteral("test_placeholder"));
    const QJsonObject warmupRequiredByRole =
        health.value(QStringLiteral("warmupRequiredByRole")).toObject();
    const QJsonObject warmupCompletedByRole =
        health.value(QStringLiteral("warmupCompletedByRole")).toObject();
    QVERIFY(warmupRequiredByRole.contains(QStringLiteral("bi-encoder")));
    QVERIFY(warmupCompletedByRole.contains(QStringLiteral("bi-encoder")));
    QVERIFY(!warmupRequiredByRole.value(QStringLiteral("bi-encoder")).toBool(true));
    QVERIFY(!warmupCompletedByRole.value(QStringLiteral("bi-encoder")).toBool(true));
}

void TestInferenceServiceContract::testSynchronousTimeoutPublishesCleanupHealth()
{
    ScopedEnvVar deterministic("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP", QByteArrayLiteral("1"));
    ScopedEnvVar placeholders("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS", QByteArrayLiteral("1"));
    ScopedEnvVar delay("BS_TEST_INFERENCE_REQUEST_DELAY_MS", QByteArrayLiteral("250"));

    bs::InferenceService service;

    QJsonObject params;
    params[QStringLiteral("query")] = QStringLiteral("slow sync request");
    params[QStringLiteral("deadlineMs")] = QDateTime::currentMSecsSinceEpoch() + 5;

    const QJsonObject timeoutPayload =
        resultPayload(service.dispatchForTest(request(20, QStringLiteral("embed_query"), params)));
    QCOMPARE(timeoutPayload.value(QStringLiteral("status")).toString(), QStringLiteral("timeout"));
    QCOMPARE(timeoutPayload.value(QStringLiteral("fallbackReason")).toString(),
             QStringLiteral("rpc_timeout"));
    QCOMPARE(timeoutPayload.value(QStringLiteral("modelRole")).toString(),
             QStringLiteral("bi-encoder"));

    QTest::qWait(350);

    const QJsonObject health =
        resultPayload(service.dispatchForTest(request(21, QStringLiteral("get_inference_health"))));
    QVERIFY(health.value(QStringLiteral("cancelSignalCount")).toInteger(0) >= 1);
    QVERIFY(health.value(QStringLiteral("timeoutCleanupCount")).toInteger(0) >= 1);

    const QJsonObject timeoutCleanupByRole =
        health.value(QStringLiteral("timeoutCleanupCountByRole")).toObject();
    QVERIFY(timeoutCleanupByRole.value(QStringLiteral("bi-encoder")).toInteger(0) >= 1);

    const QJsonObject timeoutByRole =
        health.value(QStringLiteral("timeoutCountByRole")).toObject();
    QVERIFY(timeoutByRole.value(QStringLiteral("bi-encoder")).toInteger(0) >= 1);
}

void TestInferenceServiceContract::testAvailableWorkersWarmBeforeReportingReady()
{
    const QString sourceModelsDir = bs::test::resolveModelsDirForTests();
    if (sourceModelsDir.isEmpty()) {
        QSKIP("Production fixture models are not available in this test environment");
    }

    QTemporaryDir tempModelsRoot;
    QVERIFY(tempModelsRoot.isValid());

    QString lightweightModelsError;
    const QString lightweightModelsDir =
        bs::test::prepareLightweightModelsDirForTests(
            sourceModelsDir,
            QDir(tempModelsRoot.path()).filePath(QStringLiteral("lightweight-models")),
            &lightweightModelsError);
    QVERIFY2(!lightweightModelsDir.isEmpty(), qPrintable(lightweightModelsError));

    ScopedEnvVar modelsDir("BETTERSPOTLIGHT_MODELS_DIR", lightweightModelsDir.toUtf8());
    ScopedEnvVar disableCoreMl("BETTERSPOTLIGHT_DISABLE_COREML", QByteArrayLiteral("1"));
    ScopedEnvVar deterministic("BS_TEST_INFERENCE_DETERMINISTIC_STARTUP",
                               QByteArrayLiteral("0"));
    ScopedEnvVar placeholders("BS_TEST_INFERENCE_PLACEHOLDER_WORKERS",
                              QByteArrayLiteral("0"));
    ScopedEnvVar delay("BS_TEST_INFERENCE_REQUEST_DELAY_MS", QByteArrayLiteral("0"));

    bs::InferenceService service;

    const QJsonObject health =
        resultPayload(service.dispatchForTest(request(30, QStringLiteral("get_inference_health"))));
    QVERIFY(!health.value(QStringLiteral("placeholderWorkersEnabled")).toBool(true));

    const QJsonObject statusByRole =
        health.value(QStringLiteral("roleStatusByModel")).toObject();
    const QJsonObject reasonByRole =
        health.value(QStringLiteral("roleStateReasonByModel")).toObject();
    const QJsonObject warmupRequiredByRole =
        health.value(QStringLiteral("warmupRequiredByRole")).toObject();
    const QJsonObject warmupCompletedByRole =
        health.value(QStringLiteral("warmupCompletedByRole")).toObject();
    const QJsonObject warmupElapsedByRole =
        health.value(QStringLiteral("warmupElapsedMsByRole")).toObject();
    const QJsonObject warmupFailureByRole =
        health.value(QStringLiteral("warmupFailureReasonByRole")).toObject();

    const QStringList roles = {
        QStringLiteral("bi-encoder"),
        QStringLiteral("bi-encoder-fast"),
        QStringLiteral("cross-encoder-fast"),
    };

    for (const QString& role : roles) {
        if (statusByRole.value(role).toString() != QLatin1String("ready")) {
            QSKIP(qPrintable(QStringLiteral("Fixture model role %1 is not available").arg(role)));
        }
        QCOMPARE(reasonByRole.value(role).toString(), QStringLiteral("ready"));
        QVERIFY2(warmupRequiredByRole.value(role).toBool(false),
                 qPrintable(QStringLiteral("Expected warmup to be required for %1").arg(role)));
        QVERIFY2(warmupCompletedByRole.value(role).toBool(false),
                 qPrintable(QStringLiteral("Expected warmup to complete for %1").arg(role)));
        QVERIFY2(warmupElapsedByRole.value(role).toInteger(-1) >= 0,
                 qPrintable(QStringLiteral("Expected warmup elapsed time for %1").arg(role)));
        QVERIFY2(warmupFailureByRole.value(role).toString().isEmpty(),
                 qPrintable(QStringLiteral("Unexpected warmup failure for %1: %2")
                                .arg(role,
                                     warmupFailureByRole.value(role).toString())));
    }
}

QTEST_MAIN(TestInferenceServiceContract)
#include "test_inference_service_contract.moc"
