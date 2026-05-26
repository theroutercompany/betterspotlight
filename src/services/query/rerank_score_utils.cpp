#include "rerank_score_utils.h"

#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

namespace bs::query_rerank {

namespace {

constexpr double kMaxSafeJsonInteger = 9007199254740991.0;
constexpr double kMinSafeJsonInteger = -9007199254740991.0;

void setFailureReason(QString* failureReason, const QString& reason)
{
    if (failureReason) {
        *failureReason = reason;
    }
}

std::optional<qint64> readItemId(const QJsonObject& object, QString* failureReason)
{
    const QJsonValue value = object.value(QStringLiteral("itemId"));
    if (!value.isDouble()) {
        setFailureReason(failureReason, QStringLiteral("rerank_item_id_invalid"));
        return std::nullopt;
    }

    const double raw = value.toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw
        || raw < kMinSafeJsonInteger
        || raw > kMaxSafeJsonInteger) {
        setFailureReason(failureReason, QStringLiteral("rerank_item_id_invalid"));
        return std::nullopt;
    }
    return static_cast<qint64>(raw);
}

std::optional<float> readScore(const QJsonObject& object, QString* failureReason)
{
    const QJsonValue value = object.value(QStringLiteral("score"));
    if (!value.isDouble()) {
        setFailureReason(failureReason, QStringLiteral("rerank_score_invalid"));
        return std::nullopt;
    }

    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        setFailureReason(failureReason, QStringLiteral("rerank_score_invalid"));
        return std::nullopt;
    }
    if (raw < 0.0 || raw > 1.0) {
        setFailureReason(failureReason, QStringLiteral("rerank_score_out_of_range"));
        return std::nullopt;
    }
    const float parsed = static_cast<float>(raw);
    if (!std::isfinite(parsed)) {
        setFailureReason(failureReason, QStringLiteral("rerank_score_invalid"));
        return std::nullopt;
    }
    return parsed;
}

} // namespace

std::optional<QHash<qint64, float>> parseRerankScores(const QJsonArray& scores,
                                                      QString* failureReason)
{
    QHash<qint64, float> scoreByItemId;
    scoreByItemId.reserve(scores.size());

    for (const QJsonValue& scoreValue : scores) {
        if (!scoreValue.isObject()) {
            setFailureReason(failureReason, QStringLiteral("rerank_score_row_invalid"));
            return std::nullopt;
        }

        const QJsonObject scoreObject = scoreValue.toObject();
        const std::optional<qint64> itemId = readItemId(scoreObject, failureReason);
        if (!itemId.has_value()) {
            return std::nullopt;
        }
        if (scoreByItemId.contains(*itemId)) {
            setFailureReason(failureReason, QStringLiteral("rerank_score_duplicate_item"));
            return std::nullopt;
        }

        const std::optional<float> score = readScore(scoreObject, failureReason);
        if (!score.has_value()) {
            return std::nullopt;
        }
        scoreByItemId.insert(*itemId, *score);
    }

    return scoreByItemId;
}

} // namespace bs::query_rerank
