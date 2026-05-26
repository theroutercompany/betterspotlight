#pragma once

#include <QJsonArray>
#include <QString>

#include <optional>
#include <vector>

namespace bs::query_embedding {

std::optional<std::vector<float>> parseEmbeddingVector(const QJsonArray& values,
                                                       int expectedDimensions,
                                                       QString* failureReason = nullptr);
std::optional<std::vector<std::vector<float>>> parseEmbeddingBatch(
    const QJsonArray& rows,
    size_t expectedRows,
    int expectedDimensions,
    QString* failureReason = nullptr);

} // namespace bs::query_embedding
