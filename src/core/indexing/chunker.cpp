#include "core/indexing/chunker.h"
#include "core/shared/logging.h"

namespace bs {

// ── Construction ────────────────────────────────────────────

Chunker::Chunker(const Config& config)
    : m_config(config)
{
    // Sanity-check config bounds
    if (m_config.minSize > m_config.targetSize) {
        m_config.minSize = m_config.targetSize;
    }
    if (m_config.targetSize > m_config.maxSize) {
        m_config.targetSize = m_config.maxSize;
    }
}

// ── Public API ──────────────────────────────────────────────

std::vector<Chunk> Chunker::chunkContent(const QString& filePath,
                                          const QString& content) const
{
    std::vector<Chunk> chunks;

    if (content.isEmpty()) {
        return chunks;
    }

    const auto contentLen = static_cast<size_t>(content.size());
    size_t pos = 0;
    int chunkIndex = 0;

    while (pos < contentLen) {
        size_t remaining = contentLen - pos;

        size_t chunkEnd;

        if (remaining <= m_config.targetSize) {
            // Remaining text is smaller than or equal to target — take it all
            chunkEnd = contentLen;
        } else {
            // Find a good split point near the target size
            size_t targetEnd = pos + m_config.targetSize;
            if (targetEnd > contentLen) {
                targetEnd = contentLen;
            }
            chunkEnd = findSplitPoint(content, pos, targetEnd);
        }

        // Enforce max size: force-split if the chunk exceeds maxSize
        if (chunkEnd - pos > m_config.maxSize) {
            chunkEnd = pos + m_config.maxSize;
        }

        // Enforce min size: if the remaining text after this chunk would
        // be too small, absorb it into this chunk (up to maxSize)
        if (chunkEnd < contentLen) {
            size_t leftover = contentLen - chunkEnd;
            if (leftover < m_config.minSize) {
                size_t combined = remaining;
                if (combined <= m_config.maxSize) {
                    chunkEnd = contentLen;
                }
                // else: force-split at maxSize, the leftover will form
                // its own (small) chunk — acceptable edge case
            }
        }

        Chunk c;
        c.chunkId = computeChunkId(filePath, chunkIndex);
        c.filePath = filePath;
        c.chunkIndex = chunkIndex;
        c.content = content.mid(static_cast<qsizetype>(pos),
                                static_cast<qsizetype>(chunkEnd - pos));
        c.byteOffset = pos;

        chunks.push_back(std::move(c));

        pos = chunkEnd;
        ++chunkIndex;
    }

    LOG_DEBUG(bsIndex, "Chunked %s: %d chunks from %d chars",
              qUtf8Printable(filePath),
              static_cast<int>(chunks.size()),
              static_cast<int>(contentLen));

    return chunks;
}

// ── Private helpers ─────────────────────────────────────────

size_t Chunker::findSplitPoint(const QString& content,
                                size_t chunkStart,
                                size_t targetEnd) const
{
    // Search backward from targetEnd toward chunkStart + minSize for
    // the best boundary. We try each boundary type in priority order.

    size_t searchFloor = chunkStart + m_config.minSize;
    if (searchFloor > targetEnd) {
        searchFloor = chunkStart;
    }

    // 1. Paragraph boundary: \n\n
    for (size_t i = targetEnd; i > searchFloor; --i) {
        if (i >= 2 && content[static_cast<qsizetype>(i - 1)] == QLatin1Char('\n')
            && content[static_cast<qsizetype>(i - 2)] == QLatin1Char('\n')) {
            return i;
        }
    }

    // 2. Sentence boundary: ". " or "!\n" or "?\n"
    for (size_t i = targetEnd; i > searchFloor; --i) {
        QChar prev = content[static_cast<qsizetype>(i - 1)];
        QChar curr = (i < static_cast<size_t>(content.size()))
                         ? content[static_cast<qsizetype>(i)]
                         : QLatin1Char('\0');

        if (prev == QLatin1Char('.') && curr == QLatin1Char(' ')) {
            return i;
        }
        if ((prev == QLatin1Char('!') || prev == QLatin1Char('?'))
            && curr == QLatin1Char('\n')) {
            return i;
        }
    }

    // 3. Word boundary: space
    for (size_t i = targetEnd; i > searchFloor; --i) {
        if (content[static_cast<qsizetype>(i - 1)] == QLatin1Char(' ')) {
            return i;
        }
    }

    // 4. No good boundary found — force split at targetEnd
    return targetEnd;
}

} // namespace bs
