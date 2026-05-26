#pragma once

#include "core/shared/search_result.h"

#include <QJsonArray>
#include <QString>

#include <optional>
#include <vector>

namespace bs::inference_payload {

std::optional<std::vector<QString>> parseTextBatch(const QJsonArray& texts,
                                                   QString* failureReason = nullptr);
std::optional<QJsonArray> serializeEmbedding(std::vector<float> embedding,
                                             bool normalize,
                                             QString* failureReason = nullptr);
std::optional<QJsonArray> serializeEmbeddingBatch(
    std::vector<std::vector<float>> embeddings,
    size_t expectedCount,
    bool normalize,
    QString* failureReason = nullptr);
std::optional<std::vector<SearchResult>> parseRerankCandidates(const QJsonArray& candidates,
                                                               QString* failureReason = nullptr);
std::optional<QJsonArray> serializeRerankScores(const std::vector<SearchResult>& results,
                                                QString* failureReason = nullptr);

} // namespace bs::inference_payload
