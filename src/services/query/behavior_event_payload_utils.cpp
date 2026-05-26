#include "behavior_event_payload_utils.h"

#include <QDateTime>
#include <QJsonValue>
#include <QTimeZone>

#include <cmath>
#include <limits>

namespace bs::query_behavior {

namespace {

constexpr double kMaxSafeJsonInteger = 9007199254740991.0;
constexpr double kMinSafeJsonInteger = -9007199254740991.0;
constexpr qint64 kEpochMillisCutover = 9999999999LL;

bool isMissing(const QJsonValue& value)
{
    return value.isUndefined() || value.isNull();
}

void setFailureReason(QString* failureReason, const QString& reason)
{
    if (failureReason) {
        *failureReason = reason;
    }
}

std::optional<qint64> readInteger(const QJsonValue& value,
                                  qint64 minValue,
                                  qint64 maxValue,
                                  const QString& reason,
                                  QString* failureReason)
{
    if (!value.isDouble()) {
        setFailureReason(failureReason, reason);
        return std::nullopt;
    }

    const double raw = value.toDouble();
    const double minBound = std::max(static_cast<double>(minValue), kMinSafeJsonInteger);
    const double maxBound = std::min(static_cast<double>(maxValue), kMaxSafeJsonInteger);
    if (!std::isfinite(raw) || std::floor(raw) != raw || raw < minBound || raw > maxBound) {
        setFailureReason(failureReason, reason);
        return std::nullopt;
    }

    return static_cast<qint64>(raw);
}

bool readOptionalString(const QJsonObject& object,
                        const QString& fieldName,
                        const QString& fallback,
                        const QString& reason,
                        QString* valueOut,
                        QString* failureReason)
{
    const QJsonValue value = object.value(fieldName);
    if (isMissing(value)) {
        *valueOut = fallback;
        return true;
    }
    if (!value.isString()) {
        setFailureReason(failureReason, reason);
        return false;
    }
    *valueOut = value.toString();
    return true;
}

std::optional<qint64> readOptionalNonNegativeInteger(const QJsonObject& object,
                                                     const QString& fieldName,
                                                     qint64 fallback,
                                                     const QString& reason,
                                                     QString* failureReason)
{
    const QJsonValue value = object.value(fieldName);
    if (isMissing(value)) {
        return fallback;
    }
    return readInteger(value,
                       0,
                       static_cast<qint64>(kMaxSafeJsonInteger),
                       reason,
                       failureReason);
}

std::optional<int> readOptionalNonNegativeInt(const QJsonObject& object,
                                              const QString& fieldName,
                                              int fallback,
                                              const QString& reason,
                                              QString* failureReason)
{
    const std::optional<qint64> parsed = readOptionalNonNegativeInteger(
        object,
        fieldName,
        fallback,
        reason,
        failureReason);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (*parsed > std::numeric_limits<int>::max()) {
        setFailureReason(failureReason, reason);
        return std::nullopt;
    }
    return static_cast<int>(*parsed);
}

std::optional<double> readOptionalBoundedDouble(const QJsonObject& object,
                                                const QString& fieldName,
                                                double fallback,
                                                double minValue,
                                                double maxValue,
                                                const QString& invalidReason,
                                                const QString& rangeReason,
                                                QString* failureReason)
{
    const QJsonValue value = object.value(fieldName);
    if (isMissing(value)) {
        return fallback;
    }
    if (!value.isDouble()) {
        setFailureReason(failureReason, invalidReason);
        return std::nullopt;
    }

    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        setFailureReason(failureReason, invalidReason);
        return std::nullopt;
    }
    if (raw < minValue || raw > maxValue) {
        setFailureReason(failureReason, rangeReason);
        return std::nullopt;
    }
    return raw;
}

bool readOptionalBool(const QJsonObject& object,
                      const QString& fieldName,
                      bool fallback,
                      const QString& reason,
                      bool* valueOut,
                      QString* failureReason)
{
    const QJsonValue value = object.value(fieldName);
    if (isMissing(value)) {
        *valueOut = fallback;
        return true;
    }
    if (!value.isBool()) {
        setFailureReason(failureReason, reason);
        return false;
    }
    *valueOut = value.toBool();
    return true;
}

std::optional<QJsonObject> readOptionalObject(const QJsonObject& object,
                                              const QString& fieldName,
                                              const QString& reason,
                                              QString* failureReason)
{
    const QJsonValue value = object.value(fieldName);
    if (isMissing(value)) {
        return QJsonObject();
    }
    if (!value.isObject()) {
        setFailureReason(failureReason, reason);
        return std::nullopt;
    }
    return value.toObject();
}

std::optional<QDateTime> readTimestamp(const QJsonValue& value, QString* failureReason)
{
    if (isMissing(value)) {
        return QDateTime::currentDateTimeUtc();
    }

    if (value.isString()) {
        QDateTime timestamp = QDateTime::fromString(value.toString(), Qt::ISODate);
        if (!timestamp.isValid()) {
            setFailureReason(failureReason, QStringLiteral("behavior_timestamp_invalid"));
            return std::nullopt;
        }
        return timestamp.toUTC();
    }

    const std::optional<qint64> raw = readInteger(
        value,
        0,
        static_cast<qint64>(kMaxSafeJsonInteger),
        QStringLiteral("behavior_timestamp_invalid"),
        failureReason);
    if (!raw.has_value()) {
        return std::nullopt;
    }

    const QDateTime timestamp = *raw > kEpochMillisCutover
        ? QDateTime::fromMSecsSinceEpoch(*raw, QTimeZone::UTC)
        : QDateTime::fromSecsSinceEpoch(*raw, QTimeZone::UTC);
    if (!timestamp.isValid()) {
        setFailureReason(failureReason, QStringLiteral("behavior_timestamp_invalid"));
        return std::nullopt;
    }
    return timestamp;
}

bool readInputMeta(const QJsonObject& params,
                   BehaviorEventInputMeta* inputMeta,
                   QString* failureReason)
{
    const std::optional<QJsonObject> inputObject = readOptionalObject(
        params,
        QStringLiteral("inputMeta"),
        QStringLiteral("behavior_input_meta_invalid"),
        failureReason);
    if (!inputObject.has_value()) {
        return false;
    }

    const QString reason = QStringLiteral("behavior_input_meta_invalid");
    const std::optional<int> keyEventCount = readOptionalNonNegativeInt(
        *inputObject,
        QStringLiteral("keyEventCount"),
        0,
        reason,
        failureReason);
    const std::optional<int> shortcutCount = readOptionalNonNegativeInt(
        *inputObject,
        QStringLiteral("shortcutCount"),
        0,
        reason,
        failureReason);
    const std::optional<int> scrollCount = readOptionalNonNegativeInt(
        *inputObject,
        QStringLiteral("scrollCount"),
        0,
        reason,
        failureReason);
    if (!keyEventCount.has_value() || !shortcutCount.has_value() || !scrollCount.has_value()) {
        return false;
    }

    bool metadataOnly = true;
    if (!readOptionalBool(*inputObject,
                          QStringLiteral("metadataOnly"),
                          true,
                          reason,
                          &metadataOnly,
                          failureReason)) {
        return false;
    }

    inputMeta->keyEventCount = *keyEventCount;
    inputMeta->shortcutCount = *shortcutCount;
    inputMeta->scrollCount = *scrollCount;
    inputMeta->metadataOnly = metadataOnly;
    return true;
}

bool readMouseMeta(const QJsonObject& params,
                   BehaviorEventMouseMeta* mouseMeta,
                   QString* failureReason)
{
    const std::optional<QJsonObject> mouseObject = readOptionalObject(
        params,
        QStringLiteral("mouseMeta"),
        QStringLiteral("behavior_mouse_meta_invalid"),
        failureReason);
    if (!mouseObject.has_value()) {
        return false;
    }

    const QString reason = QStringLiteral("behavior_mouse_meta_invalid");
    const std::optional<double> moveDistance = readOptionalBoundedDouble(
        *mouseObject,
        QStringLiteral("moveDistancePx"),
        0.0,
        0.0,
        std::numeric_limits<double>::max(),
        reason,
        reason,
        failureReason);
    const std::optional<int> clickCount = readOptionalNonNegativeInt(
        *mouseObject,
        QStringLiteral("clickCount"),
        0,
        reason,
        failureReason);
    const std::optional<int> dragCount = readOptionalNonNegativeInt(
        *mouseObject,
        QStringLiteral("dragCount"),
        0,
        reason,
        failureReason);
    if (!moveDistance.has_value() || !clickCount.has_value() || !dragCount.has_value()) {
        return false;
    }

    mouseMeta->moveDistancePx = *moveDistance;
    mouseMeta->clickCount = *clickCount;
    mouseMeta->dragCount = *dragCount;
    return true;
}

bool readPrivacyFlags(const QJsonObject& params,
                      BehaviorPrivacyFlags* privacyFlags,
                      QString* failureReason)
{
    const std::optional<QJsonObject> privacyObject = readOptionalObject(
        params,
        QStringLiteral("privacyFlags"),
        QStringLiteral("behavior_privacy_flags_invalid"),
        failureReason);
    if (!privacyObject.has_value()) {
        return false;
    }

    const QString reason = QStringLiteral("behavior_privacy_flags_invalid");
    return readOptionalBool(*privacyObject,
                            QStringLiteral("secureInput"),
                            false,
                            reason,
                            &privacyFlags->secureInput,
                            failureReason)
        && readOptionalBool(*privacyObject,
                            QStringLiteral("privateContext"),
                            false,
                            reason,
                            &privacyFlags->privateContext,
                            failureReason)
        && readOptionalBool(*privacyObject,
                            QStringLiteral("denylistedApp"),
                            false,
                            reason,
                            &privacyFlags->denylistedApp,
                            failureReason)
        && readOptionalBool(*privacyObject,
                            QStringLiteral("redacted"),
                            false,
                            reason,
                            &privacyFlags->redacted,
                            failureReason);
}

} // namespace

std::optional<qint64> parseRequiredPositiveItemId(const QJsonObject& params,
                                                  const QString& fieldName,
                                                  QString* failureReason)
{
    const QJsonValue value = params.value(fieldName);
    return readInteger(value,
                       1,
                       static_cast<qint64>(kMaxSafeJsonInteger),
                       QStringLiteral("selected_item_id_invalid"),
                       failureReason);
}

std::optional<int> parseOptionalNonNegativeInt(const QJsonObject& params,
                                               const QString& fieldName,
                                               int fallback,
                                               QString* failureReason)
{
    return readOptionalNonNegativeInt(params,
                                      fieldName,
                                      fallback,
                                      QStringLiteral("non_negative_int_invalid"),
                                      failureReason);
}

std::optional<BehaviorEvent> parseBehaviorEventPayload(const QJsonObject& params,
                                                       QString* failureReason)
{
    BehaviorEvent event;
    if (!readOptionalString(params,
                            QStringLiteral("eventId"),
                            QString(),
                            QStringLiteral("behavior_event_id_invalid"),
                            &event.eventId,
                            failureReason)
        || !readOptionalString(params,
                               QStringLiteral("source"),
                               QStringLiteral("betterspotlight"),
                               QStringLiteral("behavior_source_invalid"),
                               &event.source,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("eventType"),
                               QStringLiteral("activity"),
                               QStringLiteral("behavior_event_type_invalid"),
                               &event.eventType,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("appBundleId"),
                               QString(),
                               QStringLiteral("behavior_app_bundle_id_invalid"),
                               &event.appBundleId,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("windowTitleHash"),
                               QString(),
                               QStringLiteral("behavior_window_title_hash_invalid"),
                               &event.windowTitleHash,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("itemPath"),
                               QString(),
                               QStringLiteral("behavior_item_path_invalid"),
                               &event.itemPath,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("browserHostHash"),
                               QString(),
                               QStringLiteral("behavior_browser_host_hash_invalid"),
                               &event.browserHostHash,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("contextEventId"),
                               QString(),
                               QStringLiteral("behavior_context_event_id_invalid"),
                               &event.contextEventId,
                               failureReason)
        || !readOptionalString(params,
                               QStringLiteral("activityDigest"),
                               QString(),
                               QStringLiteral("behavior_activity_digest_invalid"),
                               &event.activityDigest,
                               failureReason)) {
        return std::nullopt;
    }

    const std::optional<qint64> itemId = readOptionalNonNegativeInteger(
        params,
        QStringLiteral("itemId"),
        0,
        QStringLiteral("behavior_item_id_invalid"),
        failureReason);
    if (!itemId.has_value()) {
        return std::nullopt;
    }
    event.itemId = *itemId;

    const std::optional<double> attributionConfidence = readOptionalBoundedDouble(
        params,
        QStringLiteral("attributionConfidence"),
        0.0,
        0.0,
        1.0,
        QStringLiteral("behavior_attribution_confidence_invalid"),
        QStringLiteral("behavior_attribution_confidence_out_of_range"),
        failureReason);
    if (!attributionConfidence.has_value()) {
        return std::nullopt;
    }
    event.attributionConfidence = *attributionConfidence;

    const std::optional<QDateTime> timestamp = readTimestamp(
        params.value(QStringLiteral("timestamp")),
        failureReason);
    if (!timestamp.has_value()) {
        return std::nullopt;
    }
    event.timestamp = *timestamp;

    if (!readInputMeta(params, &event.inputMeta, failureReason)
        || !readMouseMeta(params, &event.mouseMeta, failureReason)
        || !readPrivacyFlags(params, &event.privacyFlags, failureReason)) {
        return std::nullopt;
    }

    return event;
}

} // namespace bs::query_behavior
