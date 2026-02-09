#pragma once

#include <QString>

#include <optional>

namespace bs {

class DoctypeClassifier {
public:
    static std::optional<QString> classify(const QString& queryLower);
};

} // namespace bs
