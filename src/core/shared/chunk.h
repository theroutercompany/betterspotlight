#pragma once

#include <QString>
#include <string>

namespace bs {

struct Chunk {
    QString chunkId;
    QString filePath;
    int chunkIndex = 0;
    QString content;
    size_t byteOffset = 0;
};

// Compute stable chunk ID: SHA-256 of "filePath#chunkIndex" (doc 03 Stage 6)
QString computeChunkId(const QString& filePath, int chunkIndex);

} // namespace bs
