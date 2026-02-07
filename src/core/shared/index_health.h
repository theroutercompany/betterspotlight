#pragma once

#include <QString>
#include <cstdint>

namespace bs {

struct IndexHealth {
    bool isHealthy = true;
    int64_t totalIndexedItems = 0;
    double lastIndexTime = 0.0;
    double indexAge = 0.0;          // Seconds since last index
    int64_t ftsIndexSize = 0;       // Bytes
    int64_t itemsWithoutContent = 0;
    int64_t totalFailures = 0;
    int64_t totalChunks = 0;
};

} // namespace bs
