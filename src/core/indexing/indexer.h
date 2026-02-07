#pragma once

#include "core/indexing/chunker.h"
#include "core/shared/types.h"

#include <optional>
#include <string>

namespace bs {

// Forward declarations to avoid pulling in heavy headers.
class SQLiteStore;
class ExtractionManager;
class PathRules;

// Result of processing a single WorkItem through the indexing pipeline.
struct IndexResult {
    enum class Status {
        Indexed,           // Successfully indexed in FTS5
        MetadataOnly,      // Sensitive file, metadata stored but no content
        Deleted,           // Removed from index
        Excluded,          // Skipped by path rules
        ExtractionFailed,  // Content extraction failed, logged in failures table
        Skipped,           // Unchanged (same mtime + size)
    };

    Status status = Status::Excluded;
    int chunksInserted = 0;
    int chunksUpdated = 0;
    int chunksDeleted = 0;
    int durationMs = 0;
};

// Indexer — coordinates per-file processing through pipeline stages 3-7.
//
// For each WorkItem it:
//   1. Validates the path (Stage 3 — PathRules)
//   2. Extracts filesystem metadata (Stage 4 — stat())
//   3. Extracts content (Stage 5 — ExtractionManager)
//   4. Chunks content (Stage 6 — Chunker)
//   5. Inserts into FTS5 (Stage 7 — SQLiteStore)
//
// CRITICAL INVARIANT: every file that passes validation either reaches FTS5
// or is recorded as a failure. Content is never silently dropped.
class Indexer {
public:
    Indexer(SQLiteStore& store, ExtractionManager& extractor,
            const PathRules& pathRules, const Chunker& chunker);

    // Process one work item through the pipeline. Dispatches to the
    // appropriate handler based on WorkItem::Type.
    IndexResult processWorkItem(const WorkItem& item);

private:
    IndexResult processNewOrModified(const WorkItem& item);
    IndexResult processDelete(const WorkItem& item);
    IndexResult processRescan(const WorkItem& item);

    // Stage 4: Extract filesystem metadata via stat().
    // Returns nullopt if the file is inaccessible.
    std::optional<FileMetadata> extractMetadata(const std::string& filePath);

    // Stage 5+6+7: Extract content, chunk it, and insert into FTS5.
    // Enforces the critical invariant: on any failure after extraction,
    // the error is recorded via recordFailure().
    IndexResult extractAndIndex(int64_t itemId, const FileMetadata& meta);

    SQLiteStore& m_store;
    ExtractionManager& m_extractor;
    const PathRules& m_pathRules;
    const Chunker& m_chunker;
};

} // namespace bs
