#include "embedding_response_utils.h"

#include <QJsonValue>

#include <cmath>
#include <limits>
#include <utility>

namespace bs::query_embedding {

namespace {

void setFailureReason(QString* failureReason, const QString& reason)
{
    if (failureReason) {
        *failureReason = reason;
    }
}

std::optional<float> readFiniteFloat(const QJsonValue& value, QString* failureReason)
{
    if (!value.isDouble()) {
        setFailureReason(failureReason, QStringLiteral("embedding_value_invalid"));
        return std::nullopt;
    }

    const double raw = value.toDouble();
    if (!std::isfinite(raw)
        || raw < static_cast<double>(std::numeric_limits<float>::lowest())
        || raw > static_cast<double>(std::numeric_limits<float>::max())) {
        setFailureReason(failureReason, QStringLiteral("embedding_value_invalid"));
        return std::nullopt;
    }

    const float parsed = static_cast<float>(raw);
    if (!std::isfinite(parsed)) {
        setFailureReason(failureReason, QStringLiteral("embedding_value_invalid"));
        return std::nullopt;
    }
    return parsed;
}

} // namespace

std::optional<std::vector<float>> parseEmbeddingVector(const QJsonArray& values,
                                                       int expectedDimensions,
                                                       QString* failureReason)
{
    if (expectedDimensions <= 0) {
        setFailureReason(failureReason, QStringLiteral("embedding_dimension_invalid"));
        return std::nullopt;
    }
    if (values.size() != expectedDimensions) {
        setFailureReason(failureReason, QStringLiteral("embedding_dimension_mismatch"));
        return std::nullopt;
    }

    std::vector<float> out;
    out.reserve(static_cast<size_t>(values.size()));
    for (const QJsonValue& value : values) {
        const std::optional<float> parsed = readFiniteFloat(value, failureReason);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        out.push_back(*parsed);
    }
    return out;
}

std::optional<std::vector<std::vector<float>>> parseEmbeddingBatch(
    const QJsonArray& rows,
    size_t expectedRows,
    int expectedDimensions,
    QString* failureReason)
{
    if (static_cast<size_t>(rows.size()) != expectedRows) {
        setFailureReason(failureReason, QStringLiteral("embedding_row_count_mismatch"));
        return std::nullopt;
    }

    std::vector<std::vector<float>> parsedRows;
    parsedRows.reserve(static_cast<size_t>(rows.size()));
    for (const QJsonValue& rowValue : rows) {
        if (!rowValue.isArray()) {
            setFailureReason(failureReason, QStringLiteral("embedding_row_invalid"));
            return std::nullopt;
        }
        std::optional<std::vector<float>> row =
            parseEmbeddingVector(rowValue.toArray(), expectedDimensions, failureReason);
        if (!row.has_value()) {
            return std::nullopt;
        }
        parsedRows.push_back(std::move(*row));
    }
    return parsedRows;
}

} // namespace bs::query_embedding
