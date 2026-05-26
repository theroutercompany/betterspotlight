#pragma once

#include "core/learning/behavior_types.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace bs::query_behavior {

std::optional<qint64> parseRequiredPositiveItemId(const QJsonObject& params,
                                                  const QString& fieldName,
                                                  QString* failureReason = nullptr);

std::optional<int> parseOptionalNonNegativeInt(const QJsonObject& params,
                                               const QString& fieldName,
                                               int fallback,
                                               QString* failureReason = nullptr);

std::optional<BehaviorEvent> parseBehaviorEventPayload(const QJsonObject& params,
                                                       QString* failureReason = nullptr);

} // namespace bs::query_behavior
