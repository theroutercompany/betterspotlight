#include "inference_payload_utils.h"

#include <QJsonObject>
#include <QJsonValue>

#include <cmath>
#include <cstdint>
#include <utility>

namespace bs::inference_payload {

namespace {

constexpr double kMaxSafeJsonInteger = 9007199254740991.0;
constexpr double kMinSafeJsonInteger = -9007199254740991.0;

void setFailureReason(QString* failureReason, const QString& reason)
{
    if (failureReason) {
        *failureReason = reason;
    }
}

bool normalizeEmbeddingInPlace(std::vector<float>& embedding, QString* failureReason)
{
    if (embedding.empty()) {
        setFailureReason(failureReason, QStringLiteral("embedding_empty"));
        return false;
    }

    double normSquared = 0.0;
    for (const float value : embedding) {
        if (!std::isfinite(value)) {
            setFailureReason(failureReason, QStringLiteral("embedding_non_finite"));
            return false;
        }
        const double asDouble = static_cast<double>(value);
        normSquared += asDouble * asDouble;
        if (!std::isfinite(normSquared)) {
            setFailureReason(failureReason, QStringLiteral("embedding_norm_non_finite"));
            return false;
        }
    }

    const double norm = std::sqrt(normSquared);
    if (!std::isfinite(norm)) {
        setFailureReason(failureReason, QStringLiteral("embedding_norm_non_finite"));
        return false;
    }
    if (norm <= 0.0) {
        return true;
    }

    for (float& value : embedding) {
        value = static_cast<float>(static_cast<double>(value) / norm);
        if (!std::isfinite(value)) {
            setFailureReason(failureReason, QStringLiteral("embedding_non_finite"));
            return false;
        }
    }
    return true;
}

QJsonArray toJsonEmbedding(const std::vector<float>& embedding)
{
    QJsonArray out;
    for (float value : embedding) {
        out.append(static_cast<double>(value));
    }
    return out;
}

std::optional<int64_t> readIntegerId(const QJsonObject& object,
                                     const QString& key,
                                     QString* failureReason)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        setFailureReason(failureReason, QStringLiteral("candidate_item_id_invalid"));
        return std::nullopt;
    }

    const double raw = value.toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw
        || raw < kMinSafeJsonInteger
        || raw > kMaxSafeJsonInteger) {
        setFailureReason(failureReason, QStringLiteral("candidate_item_id_invalid"));
        return std::nullopt;
    }
    return static_cast<int64_t>(raw);
}

std::optional<double> readFiniteDouble(const QJsonObject& object,
                                       const QString& key,
                                       QString* failureReason)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        setFailureReason(failureReason, QStringLiteral("candidate_score_invalid"));
        return std::nullopt;
    }

    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        setFailureReason(failureReason, QStringLiteral("candidate_score_invalid"));
        return std::nullopt;
    }
    return raw;
}

} // namespace

std::optional<std::vector<QString>> parseTextBatch(const QJsonArray& texts,
                                                   QString* failureReason)
{
    std::vector<QString> parsed;
    parsed.reserve(static_cast<size_t>(texts.size()));
    for (const QJsonValue& textValue : texts) {
        if (!textValue.isString()) {
            setFailureReason(failureReason, QStringLiteral("text_not_string"));
            return std::nullopt;
        }
        parsed.push_back(textValue.toString());
    }
    return parsed;
}

std::optional<QJsonArray> serializeEmbedding(std::vector<float> embedding,
                                             bool normalize,
                                             QString* failureReason)
{
    if (embedding.empty()) {
        setFailureReason(failureReason, QStringLiteral("embedding_empty"));
        return std::nullopt;
    }
    if (normalize && !normalizeEmbeddingInPlace(embedding, failureReason)) {
        return std::nullopt;
    }
    if (!normalize) {
        for (float value : embedding) {
            if (!std::isfinite(value)) {
                setFailureReason(failureReason, QStringLiteral("embedding_non_finite"));
                return std::nullopt;
            }
        }
    }
    return toJsonEmbedding(embedding);
}

std::optional<QJsonArray> serializeEmbeddingBatch(
    std::vector<std::vector<float>> embeddings,
    size_t expectedCount,
    bool normalize,
    QString* failureReason)
{
    if (embeddings.size() != expectedCount) {
        setFailureReason(failureReason, QStringLiteral("embedding_size_mismatch"));
        return std::nullopt;
    }

    QJsonArray out;
    for (std::vector<float>& embedding : embeddings) {
        std::optional<QJsonArray> serialized =
            serializeEmbedding(std::move(embedding), normalize, failureReason);
        if (!serialized.has_value()) {
            return std::nullopt;
        }
        out.append(*serialized);
    }
    return out;
}

std::optional<std::vector<SearchResult>> parseRerankCandidates(const QJsonArray& candidates,
                                                               QString* failureReason)
{
    std::vector<SearchResult> results;
    results.reserve(static_cast<size_t>(candidates.size()));
    for (const QJsonValue& candidateValue : candidates) {
        if (!candidateValue.isObject()) {
            setFailureReason(failureReason, QStringLiteral("candidate_not_object"));
            return std::nullopt;
        }

        const QJsonObject candidate = candidateValue.toObject();
        std::optional<int64_t> itemId =
            readIntegerId(candidate, QStringLiteral("itemId"), failureReason);
        if (!itemId.has_value()) {
            return std::nullopt;
        }
        std::optional<double> score =
            readFiniteDouble(candidate, QStringLiteral("score"), failureReason);
        if (!score.has_value()) {
            return std::nullopt;
        }

        SearchResult result;
        result.itemId = *itemId;
        result.path = candidate.value(QStringLiteral("path")).toString();
        result.name = candidate.value(QStringLiteral("name")).toString();
        result.snippet = candidate.value(QStringLiteral("snippet")).toString();
        result.score = *score;
        results.push_back(std::move(result));
    }
    return results;
}

std::optional<QJsonArray> serializeRerankScores(const std::vector<SearchResult>& results,
                                                QString* failureReason)
{
    QJsonArray scores;
    for (const SearchResult& result : results) {
        if (!std::isfinite(result.crossEncoderScore)) {
            setFailureReason(failureReason, QStringLiteral("rerank_score_non_finite"));
            return std::nullopt;
        }
        if (result.crossEncoderScore < 0.0F || result.crossEncoderScore > 1.0F) {
            setFailureReason(failureReason, QStringLiteral("rerank_score_out_of_range"));
            return std::nullopt;
        }
        QJsonObject score;
        score[QStringLiteral("itemId")] = static_cast<qint64>(result.itemId);
        score[QStringLiteral("score")] = static_cast<double>(result.crossEncoderScore);
        scores.append(score);
    }
    return scores;
}

} // namespace bs::inference_payload
