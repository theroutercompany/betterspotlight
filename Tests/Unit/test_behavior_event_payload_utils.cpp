#include <QtTest/QtTest>

#include "services/query/behavior_event_payload_utils.h"

#include <QJsonObject>

class TestBehaviorEventPayloadUtils : public QObject {
    Q_OBJECT

private slots:
    void testParseBehaviorEventPayloadPreservesExplicitFieldsAndDefaults();
    void testParseBehaviorEventPayloadRejectsMalformedTypes();
    void testInteractionTelemetryIntegerParsing();
};

namespace {

QJsonObject makeValidBehaviorParams()
{
    QJsonObject params;
    params[QStringLiteral("eventId")] = QStringLiteral("event-1");
    params[QStringLiteral("eventType")] = QStringLiteral("result_open");
    params[QStringLiteral("source")] = QStringLiteral("betterspotlight");
    params[QStringLiteral("timestamp")] = qint64{1710000123};
    params[QStringLiteral("itemId")] = qint64{42};
    params[QStringLiteral("itemPath")] = QStringLiteral("/tmp/report.md");
    params[QStringLiteral("appBundleId")] = QStringLiteral("com.apple.finder");
    params[QStringLiteral("windowTitleHash")] = QStringLiteral("window-hash");
    params[QStringLiteral("browserHostHash")] = QStringLiteral("host-hash");
    params[QStringLiteral("contextEventId")] = QStringLiteral("ctx-1");
    params[QStringLiteral("activityDigest")] = QStringLiteral("digest-1");
    params[QStringLiteral("attributionConfidence")] = 0.75;

    QJsonObject inputMeta;
    inputMeta[QStringLiteral("keyEventCount")] = 3;
    inputMeta[QStringLiteral("shortcutCount")] = 1;
    inputMeta[QStringLiteral("scrollCount")] = 2;
    inputMeta[QStringLiteral("metadataOnly")] = false;
    params[QStringLiteral("inputMeta")] = inputMeta;

    QJsonObject mouseMeta;
    mouseMeta[QStringLiteral("moveDistancePx")] = 12.5;
    mouseMeta[QStringLiteral("clickCount")] = 2;
    mouseMeta[QStringLiteral("dragCount")] = 1;
    params[QStringLiteral("mouseMeta")] = mouseMeta;

    QJsonObject privacyFlags;
    privacyFlags[QStringLiteral("secureInput")] = false;
    privacyFlags[QStringLiteral("privateContext")] = true;
    privacyFlags[QStringLiteral("denylistedApp")] = false;
    privacyFlags[QStringLiteral("redacted")] = false;
    params[QStringLiteral("privacyFlags")] = privacyFlags;

    return params;
}

void expectInvalidBehaviorPayload(QJsonObject params, const QString& expectedReason)
{
    QString reason;
    const std::optional<bs::BehaviorEvent> parsed =
        bs::query_behavior::parseBehaviorEventPayload(params, &reason);
    QVERIFY(!parsed.has_value());
    QCOMPARE(reason, expectedReason);
}

} // namespace

void TestBehaviorEventPayloadUtils::testParseBehaviorEventPayloadPreservesExplicitFieldsAndDefaults()
{
    QString reason;
    const std::optional<bs::BehaviorEvent> parsed =
        bs::query_behavior::parseBehaviorEventPayload(makeValidBehaviorParams(), &reason);
    QVERIFY(parsed.has_value());
    QVERIFY(reason.isEmpty());

    QCOMPARE(parsed->eventId, QStringLiteral("event-1"));
    QCOMPARE(parsed->eventType, QStringLiteral("result_open"));
    QCOMPARE(parsed->source, QStringLiteral("betterspotlight"));
    QCOMPARE(parsed->timestamp.toSecsSinceEpoch(), qint64{1710000123});
    QCOMPARE(static_cast<qint64>(parsed->itemId), qint64{42});
    QCOMPARE(parsed->itemPath, QStringLiteral("/tmp/report.md"));
    QCOMPARE(parsed->windowTitleHash, QStringLiteral("window-hash"));
    QCOMPARE(parsed->browserHostHash, QStringLiteral("host-hash"));
    QCOMPARE(parsed->contextEventId, QStringLiteral("ctx-1"));
    QCOMPARE(parsed->activityDigest, QStringLiteral("digest-1"));
    QCOMPARE(parsed->attributionConfidence, 0.75);
    QCOMPARE(parsed->inputMeta.keyEventCount, 3);
    QCOMPARE(parsed->inputMeta.shortcutCount, 1);
    QCOMPARE(parsed->inputMeta.scrollCount, 2);
    QCOMPARE(parsed->inputMeta.metadataOnly, false);
    QCOMPARE(parsed->mouseMeta.moveDistancePx, 12.5);
    QCOMPARE(parsed->mouseMeta.clickCount, 2);
    QCOMPARE(parsed->mouseMeta.dragCount, 1);
    QCOMPARE(parsed->privacyFlags.privateContext, true);

    reason.clear();
    const std::optional<bs::BehaviorEvent> minimal =
        bs::query_behavior::parseBehaviorEventPayload(QJsonObject(), &reason);
    QVERIFY(minimal.has_value());
    QVERIFY(reason.isEmpty());
    QCOMPARE(minimal->source, QStringLiteral("betterspotlight"));
    QCOMPARE(minimal->eventType, QStringLiteral("activity"));
    QCOMPARE(static_cast<qint64>(minimal->itemId), qint64{0});
    QCOMPARE(minimal->inputMeta.metadataOnly, true);
    QCOMPARE(minimal->mouseMeta.moveDistancePx, 0.0);
    QCOMPARE(minimal->privacyFlags.redacted, false);
    QVERIFY(minimal->timestamp.isValid());
}

void TestBehaviorEventPayloadUtils::testParseBehaviorEventPayloadRejectsMalformedTypes()
{
    QJsonObject params = makeValidBehaviorParams();
    params[QStringLiteral("itemId")] = QStringLiteral("42");
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_item_id_invalid"));

    params = makeValidBehaviorParams();
    params[QStringLiteral("timestamp")] = 1710000123.5;
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_timestamp_invalid"));

    params = makeValidBehaviorParams();
    params[QStringLiteral("timestamp")] = QStringLiteral("not-a-date");
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_timestamp_invalid"));

    params = makeValidBehaviorParams();
    params[QStringLiteral("attributionConfidence")] = 1.25;
    expectInvalidBehaviorPayload(params,
                                 QStringLiteral("behavior_attribution_confidence_out_of_range"));

    params = makeValidBehaviorParams();
    params[QStringLiteral("attributionConfidence")] = QStringLiteral("0.5");
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_attribution_confidence_invalid"));

    params = makeValidBehaviorParams();
    params[QStringLiteral("inputMeta")] = QStringLiteral("not-object");
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_input_meta_invalid"));

    params = makeValidBehaviorParams();
    QJsonObject inputMeta = params.value(QStringLiteral("inputMeta")).toObject();
    inputMeta[QStringLiteral("keyEventCount")] = -1;
    params[QStringLiteral("inputMeta")] = inputMeta;
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_input_meta_invalid"));

    params = makeValidBehaviorParams();
    inputMeta = params.value(QStringLiteral("inputMeta")).toObject();
    inputMeta[QStringLiteral("metadataOnly")] = QStringLiteral("true");
    params[QStringLiteral("inputMeta")] = inputMeta;
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_input_meta_invalid"));

    params = makeValidBehaviorParams();
    QJsonObject mouseMeta = params.value(QStringLiteral("mouseMeta")).toObject();
    mouseMeta[QStringLiteral("moveDistancePx")] = -0.01;
    params[QStringLiteral("mouseMeta")] = mouseMeta;
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_mouse_meta_invalid"));

    params = makeValidBehaviorParams();
    QJsonObject privacyFlags = params.value(QStringLiteral("privacyFlags")).toObject();
    privacyFlags[QStringLiteral("redacted")] = QStringLiteral("false");
    params[QStringLiteral("privacyFlags")] = privacyFlags;
    expectInvalidBehaviorPayload(params, QStringLiteral("behavior_privacy_flags_invalid"));
}

void TestBehaviorEventPayloadUtils::testInteractionTelemetryIntegerParsing()
{
    QString reason;
    QJsonObject params;
    QVERIFY(!bs::query_behavior::parseRequiredPositiveItemId(
                 params,
                 QStringLiteral("selectedItemId"),
                 &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("selected_item_id_invalid"));

    params[QStringLiteral("selectedItemId")] = 7.5;
    reason.clear();
    QVERIFY(!bs::query_behavior::parseRequiredPositiveItemId(
                 params,
                 QStringLiteral("selectedItemId"),
                 &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("selected_item_id_invalid"));

    params[QStringLiteral("selectedItemId")] = 7;
    reason.clear();
    const std::optional<qint64> selectedId = bs::query_behavior::parseRequiredPositiveItemId(
        params,
        QStringLiteral("selectedItemId"),
        &reason);
    QVERIFY(selectedId.has_value());
    QCOMPARE(*selectedId, qint64{7});
    QVERIFY(reason.isEmpty());

    QJsonObject positionParams;
    reason.clear();
    const std::optional<int> defaultPosition = bs::query_behavior::parseOptionalNonNegativeInt(
        positionParams,
        QStringLiteral("resultPosition"),
        0,
        &reason);
    QVERIFY(defaultPosition.has_value());
    QCOMPARE(*defaultPosition, 0);
    QVERIFY(reason.isEmpty());

    positionParams[QStringLiteral("resultPosition")] = -1;
    reason.clear();
    QVERIFY(!bs::query_behavior::parseOptionalNonNegativeInt(
                 positionParams,
                 QStringLiteral("resultPosition"),
                 0,
                 &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("non_negative_int_invalid"));

    positionParams[QStringLiteral("resultPosition")] = QStringLiteral("1");
    reason.clear();
    QVERIFY(!bs::query_behavior::parseOptionalNonNegativeInt(
                 positionParams,
                 QStringLiteral("resultPosition"),
                 0,
                 &reason)
                 .has_value());
    QCOMPARE(reason, QStringLiteral("non_negative_int_invalid"));
}

QTEST_MAIN(TestBehaviorEventPayloadUtils)
#include "test_behavior_event_payload_utils.moc"
