#include <QtTest/QtTest>

#include "core/ipc/message.h"
#include "core/ipc/service_base.h"

#include <QDir>

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

class TestServiceBaseImpl final : public bs::ServiceBase {
public:
    explicit TestServiceBaseImpl(const QString& serviceName)
        : bs::ServiceBase(serviceName)
    {
    }

    QJsonObject dispatch(const QJsonObject& request)
    {
        return handleRequest(request);
    }
};

} // namespace

class TestServiceBase : public QObject {
    Q_OBJECT

private slots:
    void testRuntimeDirectoryOverrideAndPathNormalization();
    void testSocketAndPidFallbackToRuntimeDirectory();
    void testHandlePingRequest();
    void testUnknownMethodReturnsNotFoundError();
};

void TestServiceBase::testRuntimeDirectoryOverrideAndPathNormalization()
{
    const QByteArray runtimeRaw = "/tmp/bs-runtime/../bs-runtime";
    const QByteArray socketRaw = "/tmp/bs-sockets/./nested/..";
    const QByteArray pidRaw = "/tmp/bs-pids//sub/..";

    ScopedEnvVar runtimeEnv("BETTERSPOTLIGHT_RUNTIME_DIR", runtimeRaw);
    ScopedEnvVar socketEnv("BETTERSPOTLIGHT_SOCKET_DIR", socketRaw);
    ScopedEnvVar pidEnv("BETTERSPOTLIGHT_PID_DIR", pidRaw);

    QCOMPARE(bs::ServiceBase::runtimeDirectory(),
             QDir::cleanPath(QString::fromUtf8(runtimeRaw)));
    QCOMPARE(bs::ServiceBase::socketDirectory(),
             QDir::cleanPath(QString::fromUtf8(socketRaw)));
    QCOMPARE(bs::ServiceBase::pidDirectory(),
             QDir::cleanPath(QString::fromUtf8(pidRaw)));
    QCOMPARE(bs::ServiceBase::socketPath(QStringLiteral("indexer-test")),
             QDir::cleanPath(QString::fromUtf8(socketRaw) + "/indexer-test.sock"));
    QCOMPARE(bs::ServiceBase::pidPath(QStringLiteral("indexer-test")),
             QDir::cleanPath(QString::fromUtf8(pidRaw) + "/indexer-test.pid"));
}

void TestServiceBase::testSocketAndPidFallbackToRuntimeDirectory()
{
    const QByteArray runtimeRaw = "/tmp/bs-runtime-fallback/./nested/..";

    ScopedEnvVar runtimeEnv("BETTERSPOTLIGHT_RUNTIME_DIR", runtimeRaw);
    ScopedEnvVar socketEnv("BETTERSPOTLIGHT_SOCKET_DIR", QByteArray());
    ScopedEnvVar pidEnv("BETTERSPOTLIGHT_PID_DIR", QByteArray());

    const QString runtime = QDir::cleanPath(QString::fromUtf8(runtimeRaw));
    QCOMPARE(bs::ServiceBase::runtimeDirectory(), runtime);
    QCOMPARE(bs::ServiceBase::socketDirectory(), runtime);
    QCOMPARE(bs::ServiceBase::pidDirectory(), runtime);
}

void TestServiceBase::testHandlePingRequest()
{
    TestServiceBaseImpl service(QStringLiteral("service-base-unit"));
    const QJsonObject request = bs::IpcMessage::makeRequest(11, QStringLiteral("ping"));

    const QJsonObject response = service.dispatch(request);
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
    QCOMPARE(response.value(QStringLiteral("id")).toInteger(), 11);

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("pong")).toBool(), true);
    QCOMPARE(result.value(QStringLiteral("service")).toString(), QStringLiteral("service-base-unit"));
    QVERIFY(result.value(QStringLiteral("timestamp")).toInteger() > 0);
}

void TestServiceBase::testUnknownMethodReturnsNotFoundError()
{
    TestServiceBaseImpl service(QStringLiteral("service-base-unit"));
    const QJsonObject request =
        bs::IpcMessage::makeRequest(27, QStringLiteral("unknown.method"));

    const QJsonObject response = service.dispatch(request);
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("id")).toInteger(), 27);

    const QJsonObject error = response.value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toInt(),
             static_cast<int>(bs::IpcErrorCode::NotFound));
    QCOMPARE(error.value(QStringLiteral("codeString")).toString(),
             bs::ipcErrorCodeToString(bs::IpcErrorCode::NotFound));
    QVERIFY(error.value(QStringLiteral("message"))
                .toString()
                .contains(QStringLiteral("unknown.method")));
}

QTEST_MAIN(TestServiceBase)
#include "test_service_base.moc"
