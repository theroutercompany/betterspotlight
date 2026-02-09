#pragma once

#include <QString>
#include <optional>
#include <vector>

namespace bs {

// Filter options for narrowing search results.
// When all fields are empty/unset, no filtering is applied.
struct SearchOptions {
    std::vector<QString> fileTypes;      // Extensions to include (e.g. "pdf", "docx")
    std::vector<QString> excludePaths;   // Path prefixes to exclude

    std::optional<double> modifiedAfter;  // Epoch seconds — results modified after this time
    std::optional<double> modifiedBefore; // Epoch seconds — results modified before this time

    std::optional<int64_t> minSizeBytes;
    std::optional<int64_t> maxSizeBytes;

    // Returns true if any filter field is set.
    bool hasFilters() const
    {
        return !fileTypes.empty()
            || !excludePaths.empty()
            || modifiedAfter.has_value()
            || modifiedBefore.has_value()
            || minSizeBytes.has_value()
            || maxSizeBytes.has_value();
    }
};

} // namespace bs
