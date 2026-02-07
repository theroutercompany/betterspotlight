#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QtEndian>
#include "core/ipc/message.h"
#include "core/shared/ipc_messages.h"

class TestIpcMessages : public QObject {
    Q_OBJECT

private slots:
    // ── Encode/Decode roundtrip ──────────────────────────────────
    void testEncodeDecodeRoundtripRequest();
    void testEncodeDecodeRoundtripResponse();
    void testEncodeDecodeRoundtripError();
    void testEncodeDecodeRoundtripNotification();

    // ── makeRequest structure ────────────────────────────────────
    void testMakeRequestStructure();
    void testMakeRequestWithParams();
    void testMakeRequestEmptyParams();

    // ── makeResponse structure ───────────────────────────────────
    void testMakeResponseStructure();

    // ── makeError structure ──────────────────────────────────────
    void testMakeErrorStructure();
    void testMakeErrorCodeString();

    // ── makeNotification structure ───────────────────────────────
    void testMakeNotificationStructure();
    void testMakeNotificationEmptyParams();

    // ── Decode edge cases ────────────────────────────────────────
    void testDecodeIncompleteBufferLessThan4Bytes();
    void testDecodePartialMessage();
    void testDecodeMultipleMessagesConsumesOnlyFirst();
    void testDecodeEmptyBuffer();

    // ── Max message size ─────────────────────────────────────────
    void testMaxMessageSizeConstant();
    void testDecodeRejectsOversizedLength();

    // ── Unicode content ──────────────────────────────────────────
    void testUnicodeContentSurvivesRoundtrip();

    // ── Encode empty object ──────────────────────────────────────
    void testEncodeEmptyObject();

    // ── bytesConsumed ────────────────────────────────────────────
    void testBytesConsumedCorrect();
};

// ── Encode/Decode roundtrip ──────────────────────────────────────

void TestIpcMessages::testEncodeDecodeRoundtripRequest()
{
    auto req = bs::IpcMessage::makeRequest(42, QStringLiteral("searchFts5"),
        QJsonObject{{QStringLiteral("query"), QStringLiteral("hello")}});
    QByteArray encoded = bs::IpcMessage::encode(req);
    QVERIFY(!encoded.isEmpty());

    auto decoded = bs::IpcMessage::decode(encoded);
    QVERIFY(decoded.has_value());

    QCOMPARE(decoded->json[QStringLiteral("type")].toString(), QStringLiteral("request"));
    QCOMPARE(decoded->json[QStringLiteral("id")].toInteger(), 42);
    QCOMPARE(decoded->json[QStringLiteral("method")].toString(), QStringLiteral("searchFts5"));
    QCOMPARE(decoded->json[QStringLiteral("params")].toObject()[QStringLiteral("query")].toString(),
             QStringLiteral("hello"));
}

void TestIpcMessages::testEncodeDecodeRoundtripResponse()
{
    QJsonObject result;
    result[QStringLiteral("count")] = 5;
    result[QStringLiteral("status")] = QStringLiteral("ok");

    auto resp = bs::IpcMessage::makeResponse(99, result);
    QByteArray encoded = bs::IpcMessage::encode(resp);
    auto decoded = bs::IpcMessage::decode(encoded);

    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->json[QStringLiteral("type")].toString(), QStringLiteral("response"));
    QCOMPARE(decoded->json[QStringLiteral("id")].toInteger(), 99);
    QCOMPARE(decoded->json[QStringLiteral("result")].toObject()[QStringLiteral("count")].toInt(), 5);
}

void TestIpcMessages::testEncodeDecodeRoundtripError()
{
    auto err = bs::IpcMessage::makeError(
        7, bs::IpcErrorCode::NotFound, QStringLiteral("Item not found"));
    QByteArray encoded = bs::IpcMessage::encode(err);
    auto decoded = bs::IpcMessage::decode(encoded);

    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->json[QStringLiteral("type")].toString(), QStringLiteral("error"));
    QCOMPARE(decoded->json[QStringLiteral("id")].toInteger(), 7);

    auto errObj = decoded->json[QStringLiteral("error")].toObject();
    QCOMPARE(errObj[QStringLiteral("code")].toInt(),
             static_cast<int>(bs::IpcErrorCode::NotFound));
    QCOMPARE(errObj[QStringLiteral("message")].toString(), QStringLiteral("Item not found"));
}

void TestIpcMessages::testEncodeDecodeRoundtripNotification()
{
    auto notif = bs::IpcMessage::makeNotification(
        QStringLiteral("indexingProgress"),
        QJsonObject{{QStringLiteral("processed"), 42}, {QStringLiteral("total"), 100}});
    QByteArray encoded = bs::IpcMessage::encode(notif);
    auto decoded = bs::IpcMessage::decode(encoded);

    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->json[QStringLiteral("type")].toString(), QStringLiteral("notification"));
    QCOMPARE(decoded->json[QStringLiteral("method")].toString(), QStringLiteral("indexingProgress"));
}

// ── makeRequest structure ────────────────────────────────────────

void TestIpcMessages::testMakeRequestStructure()
{
    auto req = bs::IpcMessage::makeRequest(1, QStringLiteral("getHealth"));

    QVERIFY(req.contains(QStringLiteral("type")));
    QVERIFY(req.contains(QStringLiteral("id")));
    QVERIFY(req.contains(QStringLiteral("method")));
    QCOMPARE(req[QStringLiteral("type")].toString(), QStringLiteral("request"));
    QCOMPARE(req[QStringLiteral("id")].toInteger(), 1);
    QCOMPARE(req[QStringLiteral("method")].toString(), QStringLiteral("getHealth"));
}

void TestIpcMessages::testMakeRequestWithParams()
{
    QJsonObject params;
    params[QStringLiteral("query")] = QStringLiteral("test");
    params[QStringLiteral("limit")] = 10;

    auto req = bs::IpcMessage::makeRequest(5, QStringLiteral("search"), params);
    QVERIFY(req.contains(QStringLiteral("params")));
    QCOMPARE(req[QStringLiteral("params")].toObject()[QStringLiteral("query")].toString(),
             QStringLiteral("test"));
    QCOMPARE(req[QStringLiteral("params")].toObject()[QStringLiteral("limit")].toInt(), 10);
}

void TestIpcMessages::testMakeRequestEmptyParams()
{
    auto req = bs::IpcMessage::makeRequest(1, QStringLiteral("ping"));
    // Empty params should not add a "params" key
    QVERIFY(!req.contains(QStringLiteral("params")));
}

// ── makeResponse structure ───────────────────────────────────────

void TestIpcMessages::testMakeResponseStructure()
{
    QJsonObject result;
    result[QStringLiteral("data")] = QStringLiteral("value");

    auto resp = bs::IpcMessage::makeResponse(10, result);
    QCOMPARE(resp[QStringLiteral("type")].toString(), QStringLiteral("response"));
    QCOMPARE(resp[QStringLiteral("id")].toInteger(), 10);
    QVERIFY(resp.contains(QStringLiteral("result")));
    QCOMPARE(resp[QStringLiteral("result")].toObject()[QStringLiteral("data")].toString(),
             QStringLiteral("value"));
}

// ── makeError structure ──────────────────────────────────────────

void TestIpcMessages::testMakeErrorStructure()
{
    auto err = bs::IpcMessage::makeError(
        3, bs::IpcErrorCode::Timeout, QStringLiteral("Operation timed out"));

    QCOMPARE(err[QStringLiteral("type")].toString(), QStringLiteral("error"));
    QCOMPARE(err[QStringLiteral("id")].toInteger(), 3);
    QVERIFY(err.contains(QStringLiteral("error")));

    auto errObj = err[QStringLiteral("error")].toObject();
    QVERIFY(errObj.contains(QStringLiteral("code")));
    QVERIFY(errObj.contains(QStringLiteral("message")));
    QCOMPARE(errObj[QStringLiteral("code")].toInt(),
             static_cast<int>(bs::IpcErrorCode::Timeout));
    QCOMPARE(errObj[QStringLiteral("message")].toString(),
             QStringLiteral("Operation timed out"));
}

void TestIpcMessages::testMakeErrorCodeString()
{
    auto err = bs::IpcMessage::makeError(
        1, bs::IpcErrorCode::PermissionDenied, QStringLiteral("No access"));
    auto errObj = err[QStringLiteral("error")].toObject();
    QVERIFY(errObj.contains(QStringLiteral("codeString")));
    QCOMPARE(errObj[QStringLiteral("codeString")].toString(),
             QStringLiteral("PERMISSION_DENIED"));
}

// ── makeNotification structure ───────────────────────────────────

void TestIpcMessages::testMakeNotificationStructure()
{
    QJsonObject params;
    params[QStringLiteral("path")] = QStringLiteral("/test/file.txt");

    auto notif = bs::IpcMessage::makeNotification(
        QStringLiteral("fileChanged"), params);

    QCOMPARE(notif[QStringLiteral("type")].toString(), QStringLiteral("notification"));
    QCOMPARE(notif[QStringLiteral("method")].toString(), QStringLiteral("fileChanged"));
    QVERIFY(notif.contains(QStringLiteral("params")));
    // Notifications have no "id"
    QVERIFY(!notif.contains(QStringLiteral("id")));
}

void TestIpcMessages::testMakeNotificationEmptyParams()
{
    auto notif = bs::IpcMessage::makeNotification(QStringLiteral("heartbeat"));
    QVERIFY(!notif.contains(QStringLiteral("params")));
}

// ── Decode edge cases ────────────────────────────────────────────

void TestIpcMessages::testDecodeIncompleteBufferLessThan4Bytes()
{
    QByteArray buf;
    buf.append('\x00');
    buf.append('\x00');
    auto result = bs::IpcMessage::decode(buf);
    QVERIFY(!result.has_value());
}

void TestIpcMessages::testDecodePartialMessage()
{
    // Create a valid encoded message, then truncate it
    auto req = bs::IpcMessage::makeRequest(1, QStringLiteral("test"));
    QByteArray encoded = bs::IpcMessage::encode(req);
    // Take only the length header + half the payload
    QByteArray partial = encoded.left(4 + encoded.size() / 4);
    auto result = bs::IpcMessage::decode(partial);
    QVERIFY(!result.has_value());
}

void TestIpcMessages::testDecodeMultipleMessagesConsumesOnlyFirst()
{
    auto req1 = bs::IpcMessage::makeRequest(1, QStringLiteral("first"));
    auto req2 = bs::IpcMessage::makeRequest(2, QStringLiteral("second"));

    QByteArray combined = bs::IpcMessage::encode(req1);
    QByteArray second = bs::IpcMessage::encode(req2);
    combined.append(second);

    auto result1 = bs::IpcMessage::decode(combined);
    QVERIFY(result1.has_value());
    QCOMPARE(result1->json[QStringLiteral("method")].toString(), QStringLiteral("first"));

    // bytesConsumed should only cover the first message
    QVERIFY(result1->bytesConsumed < combined.size());

    // Decode the remainder
    QByteArray remaining = combined.mid(result1->bytesConsumed);
    auto result2 = bs::IpcMessage::decode(remaining);
    QVERIFY(result2.has_value());
    QCOMPARE(result2->json[QStringLiteral("method")].toString(), QStringLiteral("second"));
}

void TestIpcMessages::testDecodeEmptyBuffer()
{
    auto result = bs::IpcMessage::decode(QByteArray());
    QVERIFY(!result.has_value());
}

// ── Max message size ─────────────────────────────────────────────

void TestIpcMessages::testMaxMessageSizeConstant()
{
    QCOMPARE(bs::IpcMessage::kMaxMessageSize, 16 * 1024 * 1024);
}

void TestIpcMessages::testDecodeRejectsOversizedLength()
{
    // Create a buffer with a length prefix that exceeds kMaxMessageSize
    QByteArray buf;
    buf.resize(4);
    quint32 hugeLen = qToBigEndian(static_cast<quint32>(20 * 1024 * 1024)); // 20MB
    memcpy(buf.data(), &hugeLen, 4);
    // Add some dummy data
    buf.append(QByteArray(100, 'x'));

    auto result = bs::IpcMessage::decode(buf);
    QVERIFY(!result.has_value());
}

// ── Unicode content ──────────────────────────────────────────────

void TestIpcMessages::testUnicodeContentSurvivesRoundtrip()
{
    QJsonObject params;
    params[QStringLiteral("query")] = QStringLiteral("\xC3\xA9\xC3\xA0\xC3\xBC \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
    params[QStringLiteral("emoji")] = QStringLiteral("\xF0\x9F\x98\x80\xF0\x9F\x8E\x89");

    auto req = bs::IpcMessage::makeRequest(1, QStringLiteral("search"), params);
    QByteArray encoded = bs::IpcMessage::encode(req);
    auto decoded = bs::IpcMessage::decode(encoded);

    QVERIFY(decoded.has_value());
    auto decodedParams = decoded->json[QStringLiteral("params")].toObject();
    QCOMPARE(decodedParams[QStringLiteral("query")].toString(),
             QStringLiteral("\xC3\xA9\xC3\xA0\xC3\xBC \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));
    QCOMPARE(decodedParams[QStringLiteral("emoji")].toString(),
             QStringLiteral("\xF0\x9F\x98\x80\xF0\x9F\x8E\x89"));
}

// ── Encode empty object ──────────────────────────────────────────

void TestIpcMessages::testEncodeEmptyObject()
{
    QJsonObject empty;
    QByteArray encoded = bs::IpcMessage::encode(empty);
    QVERIFY(!encoded.isEmpty());

    auto decoded = bs::IpcMessage::decode(encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(decoded->json.isEmpty());
}

// ── bytesConsumed ────────────────────────────────────────────────

void TestIpcMessages::testBytesConsumedCorrect()
{
    auto req = bs::IpcMessage::makeRequest(1, QStringLiteral("test"));
    QByteArray encoded = bs::IpcMessage::encode(req);

    auto decoded = bs::IpcMessage::decode(encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->bytesConsumed, static_cast<int>(encoded.size()));
}

QTEST_MAIN(TestIpcMessages)
#include "test_ipc_messages.moc"
