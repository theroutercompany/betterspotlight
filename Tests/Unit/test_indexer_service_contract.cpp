#include <QtTest/QtTest>

#include "core/ipc/message.h"
#include "services/indexer/indexer_service.h"

#include <QDir>
#include <QFile>
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

class TestableIndexerService final : public bs::IndexerService {
public:
    QJsonObject dispatch(const QJsonObject& request)
    {
        return handleRequest(request);
    }
};

QJsonObject request(uint64_t id, const QString& method, const QJsonObject& params = {})
{
    return bs::IpcMessage::makeRequest(id, method, params);
}

QJsonObject expectResponse(const QJsonObject& response)
{
    if (response.value(QStringLiteral("type")).toString() != QLatin1String("response")) {
        QTest::qFail("Expected IPC response envelope", __FILE__, __LINE__);
        return {};
    }
    return response.value(QStringLiteral("result")).toObject();
}

QJsonObject expectError(const QJsonObject& response, bs::IpcErrorCode expectedCode)
{
    if (response.value(QStringLiteral("type")).toString() != QLatin1String("error")) {
        QTest::qFail("Expected IPC error envelope", __FILE__, __LINE__);
        return {};
    }
    const QJsonObject error = response.value(QStringLiteral("error")).toObject();
    if (error.value(QStringLiteral("code")).toInt() != static_cast<int>(expectedCode)
        || error.value(QStringLiteral("codeString")).toString()
               != bs::ipcErrorCodeToString(expectedCode)) {
        QTest::qFail("Unexpected IPC error code", __FILE__, __LINE__);
        return error;
    }
    if (error.value(QStringLiteral("message")).toString().isEmpty()) {
        QTest::qFail("Expected non-empty IPC error message", __FILE__, __LINE__);
        return error;
    }
    return error;
}

QJsonObject startParamsForRoot(const QString& root)
{
    QJsonArray roots;
    roots.append(root);

    QJsonObject params;
    params[QStringLiteral("roots")] = roots;
    return params;
}

} // namespace

class TestIndexerServiceContract : public QObject {
    Q_OBJECT

private slots:
    void testPreStartRequestsReturnObservableErrorsAndQueueTelemetry();
    void testStartIndexingValidatesRootsAndMaintainsState();
    void testSetUserActiveRequiresBooleanPayload();
};

void TestIndexerServiceContract::testPreStartRequestsReturnObservableErrorsAndQueueTelemetry()
{
    TestableIndexerService service;

    const QJsonObject queue =
        expectResponse(service.dispatch(request(1, QStringLiteral("getQueueStatus"))));
    QCOMPARE(queue.value(QStringLiteral("pending")).toInteger(), qint64{0});
    QCOMPARE(queue.value(QStringLiteral("processing")).toInteger(), qint64{0});
    QCOMPARE(queue.value(QStringLiteral("failed")).toInteger(), qint64{0});
    QCOMPARE(queue.value(QStringLiteral("paused")).toBool(true), false);
    QCOMPARE(queue.value(QStringLiteral("rebuildRunning")).toBool(true), false);
    QCOMPARE(queue.value(QStringLiteral("rebuildStatus")).toString(), QStringLiteral("idle"));
    QVERIFY(queue.value(QStringLiteral("roots")).toArray().isEmpty());
    QVERIFY(queue.value(QStringLiteral("memory")).toObject().contains(QStringLiteral("rssMb")));
    QVERIFY(!queue.value(QStringLiteral("actorMode")).toString().isEmpty());
    QVERIFY(queue.value(QStringLiteral("bulkhead")).toObject().isEmpty());

    expectError(service.dispatch(request(2, QStringLiteral("pauseIndexing"))),
                bs::IpcErrorCode::InvalidParams);
    expectError(service.dispatch(request(3, QStringLiteral("resumeIndexing"))),
                bs::IpcErrorCode::InvalidParams);
    expectError(service.dispatch(request(4, QStringLiteral("setUserActive"))),
                bs::IpcErrorCode::InvalidParams);
    expectError(service.dispatch(request(5, QStringLiteral("reindexPath"))),
                bs::IpcErrorCode::InvalidParams);
    expectError(service.dispatch(request(6, QStringLiteral("rebuildAll"))),
                bs::IpcErrorCode::InvalidParams);
    expectError(service.dispatch(request(7, QStringLiteral("unknownIndexerMethod"))),
                bs::IpcErrorCode::NotFound);
}

void TestIndexerServiceContract::testStartIndexingValidatesRootsAndMaintainsState()
{
    QTemporaryDir tempHome;
    QTemporaryDir dataDir;
    QTemporaryDir rootDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(dataDir.isValid());
    QVERIFY(rootDir.isValid());

    ScopedEnvVar homeEnv("HOME", tempHome.path().toUtf8());
    ScopedEnvVar fixedHomeEnv("CFFIXED_USER_HOME", tempHome.path().toUtf8());
    ScopedEnvVar dataEnv("BETTERSPOTLIGHT_DATA_DIR", dataDir.path().toUtf8());

    const QString bsignorePath = QDir(tempHome.path()).filePath(QStringLiteral(".bsignore"));
    {
        QFile bsignore(bsignorePath);
        QVERIFY(bsignore.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bsignore.write("*.tmp\nbuild/\n");
    }

    const QString fixturePath = QDir(rootDir.path()).filePath(QStringLiteral("contract.txt"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fixture.write("indexer service direct contract fixture\n");
    }

    TestableIndexerService service;

    expectError(service.dispatch(request(10, QStringLiteral("startIndexing"))),
                bs::IpcErrorCode::InvalidParams);

    QJsonObject invalidParams;
    invalidParams[QStringLiteral("roots")] = QJsonArray{42, QString()};
    expectError(service.dispatch(request(11, QStringLiteral("startIndexing"), invalidParams)),
                bs::IpcErrorCode::InvalidParams);

    const QJsonObject start =
        expectResponse(service.dispatch(request(12,
                                                QStringLiteral("startIndexing"),
                                                startParamsForRoot(rootDir.path()))));
    QCOMPARE(start.value(QStringLiteral("success")).toBool(false), true);
    QVERIFY(start.value(QStringLiteral("timestamp")).toInteger() > 0);
    QVERIFY(start.contains(QStringLiteral("queuedPaths")));

    expectError(service.dispatch(request(13,
                                         QStringLiteral("startIndexing"),
                                         startParamsForRoot(rootDir.path()))),
                bs::IpcErrorCode::AlreadyRunning);

    const QJsonObject paused =
        expectResponse(service.dispatch(request(14, QStringLiteral("pauseIndexing"))));
    QCOMPARE(paused.value(QStringLiteral("paused")).toBool(false), true);

    const QJsonObject resumed =
        expectResponse(service.dispatch(request(15, QStringLiteral("resumeIndexing"))));
    QCOMPARE(resumed.value(QStringLiteral("resumed")).toBool(false), true);

    QJsonObject activeParams;
    activeParams[QStringLiteral("active")] = true;
    const QJsonObject active =
        expectResponse(service.dispatch(request(16, QStringLiteral("setUserActive"), activeParams)));
    QCOMPARE(active.value(QStringLiteral("active")).toBool(false), true);
    QVERIFY(active.contains(QStringLiteral("prepWorkers")));

    expectError(service.dispatch(request(17, QStringLiteral("reindexPath"))),
                bs::IpcErrorCode::InvalidParams);

    QJsonObject reindexParams;
    reindexParams[QStringLiteral("path")] = fixturePath;
    const QJsonObject reindex =
        expectResponse(service.dispatch(request(18, QStringLiteral("reindexPath"), reindexParams)));
    QCOMPARE(reindex.value(QStringLiteral("queued")).toBool(false), true);

    const QJsonObject queue =
        expectResponse(service.dispatch(request(19, QStringLiteral("getQueueStatus"))));
    QCOMPARE(queue.value(QStringLiteral("roots")).toArray().size(), 1);
    QCOMPARE(queue.value(QStringLiteral("roots")).toArray().at(0).toString(), rootDir.path());
    QCOMPARE(queue.value(QStringLiteral("bsignoreFileExists")).toBool(false), true);
    QCOMPARE(queue.value(QStringLiteral("bsignoreLoaded")).toBool(false), true);
    QCOMPARE(queue.value(QStringLiteral("bsignorePatternCount")).toInt(0), 2);
    QVERIFY(queue.value(QStringLiteral("lastProgressReport")).toObject()
                .contains(QStringLiteral("scanned")));
    QVERIFY(queue.value(QStringLiteral("bulkhead")).isObject());
}

void TestIndexerServiceContract::testSetUserActiveRequiresBooleanPayload()
{
    QTemporaryDir tempHome;
    QTemporaryDir dataDir;
    QTemporaryDir rootDir;
    QVERIFY(tempHome.isValid());
    QVERIFY(dataDir.isValid());
    QVERIFY(rootDir.isValid());

    ScopedEnvVar homeEnv("HOME", tempHome.path().toUtf8());
    ScopedEnvVar fixedHomeEnv("CFFIXED_USER_HOME", tempHome.path().toUtf8());
    ScopedEnvVar dataEnv("BETTERSPOTLIGHT_DATA_DIR", dataDir.path().toUtf8());

    TestableIndexerService service;
    expectResponse(service.dispatch(request(30,
                                            QStringLiteral("startIndexing"),
                                            startParamsForRoot(rootDir.path()))));

    QJsonObject stringActive;
    stringActive[QStringLiteral("active")] = QStringLiteral("true");
    const QJsonObject stringError =
        expectError(service.dispatch(request(31, QStringLiteral("setUserActive"), stringActive)),
                    bs::IpcErrorCode::InvalidParams);
    QVERIFY(stringError.value(QStringLiteral("message"))
                .toString()
                .contains(QStringLiteral("boolean")));

    QJsonObject numericActive;
    numericActive[QStringLiteral("active")] = 1;
    expectError(service.dispatch(request(32, QStringLiteral("setUserActive"), numericActive)),
                bs::IpcErrorCode::InvalidParams);

    QJsonObject validActive;
    validActive[QStringLiteral("active")] = false;
    const QJsonObject valid =
        expectResponse(service.dispatch(request(33, QStringLiteral("setUserActive"), validActive)));
    QCOMPARE(valid.value(QStringLiteral("active")).toBool(true), false);
}

QTEST_MAIN(TestIndexerServiceContract)
#include "test_indexer_service_contract.moc"
