#pragma once

#include "core/indexing/chunker.h"
#include "core/extraction/extractor.h"
#include "core/shared/types.h"

#include <optional>
#include <string>
#include <vector>

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

struct PreparedFailure {
    QString stage;
    QString message;
    std::optional<ExtractionResult::Status> extractionStatus;
};

// PreparedWork is produced by parallel prep workers and later consumed
// by the single-threaded DB writer stage.
struct PreparedWork {
    WorkItem::Type type = WorkItem::Type::NewFile;
    QString path;
    uint64_t generation = 0;
    int retryCount = 0;

    ValidationResult validation = ValidationResult::Include;
    std::optional<FileMetadata> metadata;
    QString parentPath;
    Sensitivity sensitivity = Sensitivity::Normal;

    bool nonExtractable = false;
    bool hasExtractedContent = false;
    QString contentHash;
    std::vector<Chunk> chunks;

    std::optional<PreparedFailure> failure;
    int prepDurationMs = 0;
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

    // Prep stage: CPU/IO-heavy work that does not mutate SQLite.
    PreparedWork prepareWorkItem(const WorkItem& item, uint64_t generation = 0);

    // Writer stage: applies a prepared unit to SQLite (single-threaded owner).
    IndexResult applyPreparedWork(const PreparedWork& prepared);

    // Process one work item through the pipeline. Dispatches to the
    // staged prepare+apply flow. Kept for compatibility with existing tests.
    IndexResult processWorkItem(const WorkItem& item);

private:
    PreparedWork prepareNewOrModified(const WorkItem& item, uint64_t generation);
    PreparedWork prepareDelete(const WorkItem& item, uint64_t generation);
    PreparedWork prepareRescan(const WorkItem& item, uint64_t generation);

    IndexResult applyNewOrModified(const PreparedWork& prepared);
    IndexResult applyDelete(const PreparedWork& prepared);
    IndexResult applyRescan(const PreparedWork& prepared);

    // Stage 4: Extract filesystem metadata via stat().
    // Returns nullopt if the file is inaccessible.
    std::optional<FileMetadata> extractMetadata(const std::string& filePath);

    // Prep-only extraction + chunking stage.
    void prepareExtractedContent(PreparedWork& prepared, const FileMetadata& meta,
                                 int initialRetryCount);

    SQLiteStore& m_store;
    ExtractionManager& m_extractor;
    const PathRules& m_pathRules;
    const Chunker& m_chunker;
};

} // namespace bs
