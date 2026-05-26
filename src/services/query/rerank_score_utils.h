#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>

#include <optional>

namespace bs::query_rerank {

std::optional<QHash<qint64, float>> parseRerankScores(const QJsonArray& scores,
                                                      QString* failureReason = nullptr);

} // namespace bs::query_rerank
