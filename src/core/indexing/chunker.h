#pragma once

#include "core/shared/chunk.h"

#include <QString>
#include <cstddef>
#include <vector>

namespace bs {

// Configuration for the Chunker.
// Defined outside the class to avoid the "default member initializer needed
// within enclosing class" issue in C++.
struct ChunkerConfig {
    size_t targetSize = 1000;
    size_t minSize = 500;
    size_t maxSize = 2000;
};

// Chunker — splits extracted text content into sized chunks for FTS5 indexing.
//
// Split priority (highest to lowest):
//   1. Paragraph boundary (\n\n)
//   2. Sentence boundary (. followed by space, or !  ?, followed by newline)
//   3. Word boundary (space)
//   4. Force character split at maxSize
//
// Chunks do not overlap (FTS5 does not need overlap).
// Each chunk receives a stable ID via computeChunkId(filePath, chunkIndex).
class Chunker {
public:
    using Config = ChunkerConfig;

    explicit Chunker(const Config& config = {});

    // Split content into chunks. Each chunk gets a stable ID via computeChunkId.
    // Returns an empty vector if content is empty.
    std::vector<Chunk> chunkContent(const QString& filePath, const QString& content) const;

private:
    // Find the best split point near targetEnd, searching backward from targetEnd.
    // Returns the index *after* the split (i.e., the start of the next chunk).
    size_t findSplitPoint(const QString& content, size_t chunkStart, size_t targetEnd) const;

    Config m_config;
};

} // namespace bs
