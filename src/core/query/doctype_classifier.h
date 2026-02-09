#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace bs {

class DoctypeClassifier {
public:
    static std::optional<QString> classify(const QString& queryLower);

    // Maps a doctype intent to file extensions that match it.
    // Returns empty vector if intent is unknown.
    static std::vector<QString> extensionsForIntent(const QString& intent);
};

} // namespace bs
