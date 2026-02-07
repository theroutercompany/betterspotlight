#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

namespace bs {

struct Settings {
    // Database
    QString dbPath;

    // Indexing paths
    QStringList indexPaths;
    QStringList excludePatterns;

    // Extraction limits
    int64_t maxFileSize = 104857600;         // 100 MB
    uint32_t extractionTimeoutMs = 5000;

    // Chunking
    uint32_t chunkSizeBytes = 4096;

    // Embedding (M2)
    bool embeddingEnabled = false;
};

} // namespace bs
