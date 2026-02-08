#include "core/indexing/indexer.h"
#include "core/index/sqlite_store.h"
#include "core/extraction/extraction_manager.h"
#include "core/extraction/extractor.h"
#include "core/fs/file_scanner.h"
#include "core/fs/path_rules.h"
#include "core/shared/logging.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>

#include <sys/stat.h>
#include <unistd.h>

namespace bs {

// ── Construction ────────────────────────────────────────────

Indexer::Indexer(SQLiteStore& store, ExtractionManager& extractor,
                const PathRules& pathRules, const Chunker& chunker)
    : m_store(store)
    , m_extractor(extractor)
    , m_pathRules(pathRules)
    , m_chunker(chunker)
{
    LOG_INFO(bsIndex, "Indexer initialised");
}

// ── Public entry point ──────────────────────────────────────

IndexResult Indexer::processWorkItem(const WorkItem& item)
{
    switch (item.type) {
    case WorkItem::Type::Delete:
        return processDelete(item);
    case WorkItem::Type::ModifiedContent:
    case WorkItem::Type::NewFile:
        return processNewOrModified(item);
    case WorkItem::Type::RescanDirectory:
        return processRescan(item);
    }

    // Unreachable, but satisfies compiler warnings
    IndexResult result;
    result.status = IndexResult::Status::Excluded;
    return result;
}

// ── Delete ──────────────────────────────────────────────────

IndexResult Indexer::processDelete(const WorkItem& item)
{
    QElapsedTimer timer;
    timer.start();

    IndexResult result;
    result.status = IndexResult::Status::Deleted;

    const QString path = QString::fromStdString(item.filePath);

    auto existing = m_store.getItemByPath(path);
    if (existing.has_value()) {
        // Delete chunks first, then the item
        m_store.deleteChunksForItem(existing->id, path);
        m_store.deleteItemByPath(path);
        LOG_INFO(bsIndex, "Deleted from index: %s (id=%lld)",
                 item.filePath.c_str(), static_cast<long long>(existing->id));
    } else {
        LOG_DEBUG(bsIndex, "Delete requested for non-indexed path: %s",
                  item.filePath.c_str());
    }

    result.durationMs = static_cast<int>(timer.elapsed());
    return result;
}

// ── New or modified file ────────────────────────────────────

IndexResult Indexer::processNewOrModified(const WorkItem& item)
{
    QElapsedTimer timer;
    timer.start();

    IndexResult result;

    // Stage 3: Path validation
    auto validation = m_pathRules.validate(item.filePath);

    if (validation == ValidationResult::Exclude) {
        result.status = IndexResult::Status::Excluded;
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_DEBUG(bsIndex, "Excluded by path rules: %s", item.filePath.c_str());
        return result;
    }

    // Stage 4: Extract filesystem metadata
    auto meta = extractMetadata(item.filePath);
    if (!meta.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_WARN(bsIndex, "Cannot stat file: %s", item.filePath.c_str());
        return result;
    }

    // Skip check: if the file has not changed (same mtime + size)
    const QString qPath = QString::fromStdString(item.filePath);
    auto existing = m_store.getItemByPath(qPath);
    if (existing.has_value() && item.type == WorkItem::Type::ModifiedContent) {
        if (static_cast<int64_t>(meta->fileSize) == existing->size
            && meta->modifiedAt == existing->modifiedAt) {
            result.status = IndexResult::Status::Skipped;
            result.durationMs = static_cast<int>(timer.elapsed());
            LOG_DEBUG(bsIndex, "Skipped (unchanged): %s", item.filePath.c_str());
            return result;
        }
    }

    // Determine sensitivity
    Sensitivity sensitivity = m_pathRules.classifySensitivity(item.filePath);
    QString sensitivityStr = sensitivityToString(sensitivity);

    // Compute parent path
    QFileInfo fi(qPath);
    QString parentPath = fi.path();

    // Upsert the item metadata in the store
    auto itemId = m_store.upsertItem(
        qPath,
        QString::fromStdString(meta->fileName),
        QString::fromStdString(meta->extension),
        meta->itemKind,
        static_cast<int64_t>(meta->fileSize),
        meta->createdAt,
        meta->modifiedAt,
        QString(),   // contentHash — set after extraction
        sensitivityStr,
        parentPath);

    if (!itemId.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_ERROR(bsIndex, "Failed to upsert item metadata: %s", item.filePath.c_str());
        return result;
    }

    // MetadataOnly: sensitive files get metadata but no content extraction
    if (validation == ValidationResult::MetadataOnly) {
        result.status = IndexResult::Status::MetadataOnly;
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_INFO(bsIndex, "MetadataOnly (sensitive): %s", item.filePath.c_str());
        return result;
    }

    // Non-extractable item kinds: metadata is already stored; no content to extract.
    // This is NOT a failure — Binary/Archive/Unknown/Directory items are findable
    // by name/path but have no text content for FTS5.
    if (meta->itemKind == ItemKind::Directory
        || meta->itemKind == ItemKind::Archive
        || meta->itemKind == ItemKind::Binary
        || meta->itemKind == ItemKind::Unknown) {
        result.status = IndexResult::Status::Indexed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    static constexpr int kMaxRetries = 2;
    int attempts = item.retryCount;
    do {
        result = extractAndIndex(itemId.value(), meta.value());
        if (result.status != IndexResult::Status::ExtractionFailed) break;
        attempts++;
        if (attempts <= kMaxRetries) {
            LOG_WARN(bsIndex, "Retrying extraction (%d/%d): %s",
                     attempts, kMaxRetries, item.filePath.c_str());
        }
    } while (attempts <= kMaxRetries);

    result.durationMs = static_cast<int>(timer.elapsed());
    return result;
}

// ── Rescan directory ────────────────────────────────────────

IndexResult Indexer::processRescan(const WorkItem& item)
{
    // A RescanDirectory item is treated as a NewFile for the directory path
    // itself. The actual directory walk is handled by the Pipeline, which
    // will enqueue individual NewFile items for each discovered file.
    //
    // Here we just validate the directory path and update its metadata.
    QElapsedTimer timer;
    timer.start();

    IndexResult result;

    auto validation = m_pathRules.validate(item.filePath);
    if (validation == ValidationResult::Exclude) {
        result.status = IndexResult::Status::Excluded;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    auto meta = extractMetadata(item.filePath);
    if (!meta.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    const QString qPath = QString::fromStdString(item.filePath);
    QFileInfo fi(qPath);
    QString parentPath = fi.path();
    Sensitivity sensitivity = m_pathRules.classifySensitivity(item.filePath);

    m_store.upsertItem(
        qPath,
        QString::fromStdString(meta->fileName),
        QString::fromStdString(meta->extension),
        meta->itemKind,
        static_cast<int64_t>(meta->fileSize),
        meta->createdAt,
        meta->modifiedAt,
        QString(),
        sensitivityToString(sensitivity),
        parentPath);

    result.status = IndexResult::Status::Indexed;
    result.durationMs = static_cast<int>(timer.elapsed());
    return result;
}

// ── Stage 4: Metadata extraction ────────────────────────────

std::optional<FileMetadata> Indexer::extractMetadata(const std::string& filePath)
{
    struct stat st;
    if (stat(filePath.c_str(), &st) != 0) {
        return std::nullopt;
    }

    if (access(filePath.c_str(), R_OK) != 0) {
        return std::nullopt;
    }

    QFileInfo fi(QString::fromStdString(filePath));

    FileMetadata meta;
    meta.filePath = filePath;
    meta.fileName = fi.fileName().toStdString();
    QString ext = fi.suffix().toLower();
    meta.extension = ext.isEmpty() ? std::string() : ("." + ext).toStdString();
    meta.fileSize = static_cast<uint64_t>(st.st_size);

#if defined(__APPLE__)
    meta.createdAt = static_cast<double>(st.st_birthtimespec.tv_sec)
                   + static_cast<double>(st.st_birthtimespec.tv_nsec) / 1e9;
    meta.modifiedAt = static_cast<double>(st.st_mtimespec.tv_sec)
                    + static_cast<double>(st.st_mtimespec.tv_nsec) / 1e9;
#else
    meta.createdAt = static_cast<double>(st.st_ctime);
    meta.modifiedAt = static_cast<double>(st.st_mtim.tv_sec)
                    + static_cast<double>(st.st_mtim.tv_nsec) / 1e9;
#endif

    meta.permissions = static_cast<uint16_t>(st.st_mode & 0777);
    meta.isReadable = true;

    // Classify ItemKind from extension and mode
    // Directories get ItemKind::Directory
    if (S_ISDIR(st.st_mode)) {
        meta.itemKind = ItemKind::Directory;
    } else {
        meta.itemKind = FileScanner::classifyItemKind(meta.extension, st.st_mode);
    }

    return meta;
}

// ── Stages 5+6+7: Extract, chunk, index ─────────────────────

IndexResult Indexer::extractAndIndex(int64_t itemId, const FileMetadata& meta)
{
    IndexResult result;

    const QString qPath = QString::fromStdString(meta.filePath);

    // Stage 5: Content extraction
    ExtractionResult extraction = m_extractor.extract(qPath, meta.itemKind);

    if (extraction.status != ExtractionResult::Status::Success
        || !extraction.content.has_value()) {
        // CRITICAL INVARIANT: record the failure, never silently drop
        QString errorMsg = extraction.errorMessage.value_or(
            QStringLiteral("Extraction failed with no details"));
        m_store.recordFailure(itemId, QStringLiteral("extraction"), errorMsg);

        result.status = IndexResult::Status::ExtractionFailed;
        LOG_WARN(bsIndex, "Extraction failed for %s: %s",
                 meta.filePath.c_str(), qUtf8Printable(errorMsg));
        return result;
    }

    const QString& content = extraction.content.value();

    const QByteArray contentUtf8 = content.toUtf8();
    const QString newHash = QString::fromLatin1(
        QCryptographicHash::hash(contentUtf8, QCryptographicHash::Sha256).toHex());

    auto existingItem = m_store.getItemById(itemId);
    if (existingItem.has_value() && !existingItem->contentHash.isEmpty()
        && existingItem->contentHash == newHash) {
        result.status = IndexResult::Status::Skipped;
        LOG_DEBUG(bsIndex, "Skipped (content hash unchanged): %s", meta.filePath.c_str());
        return result;
    }

    std::vector<Chunk> newChunks = m_chunker.chunkContent(qPath, content);

    if (newChunks.empty()) {
        m_store.updateContentHash(itemId, newHash);
        result.status = IndexResult::Status::Indexed;
        LOG_DEBUG(bsIndex, "No chunks produced for %s (empty content)",
                  meta.filePath.c_str());
        return result;
    }

    bool insertOk = m_store.insertChunks(itemId,
                                          QString::fromStdString(meta.fileName),
                                          qPath,
                                          newChunks);

    if (!insertOk) {
        m_store.recordFailure(itemId, QStringLiteral("fts5_insert"),
                              QStringLiteral("insertChunks() returned false"));
        result.status = IndexResult::Status::ExtractionFailed;
        LOG_ERROR(bsIndex, "CRITICAL: insertChunks failed for %s (id=%lld) — recorded failure",
                  meta.filePath.c_str(), static_cast<long long>(itemId));
        return result;
    }

    m_store.updateContentHash(itemId, newHash);

    result.status = IndexResult::Status::Indexed;
    result.chunksInserted = static_cast<int>(newChunks.size());

    LOG_INFO(bsIndex, "Indexed %s: %d chunks",
             meta.filePath.c_str(), result.chunksInserted);

    m_store.clearFailures(itemId);

    return result;
}

} // namespace bs
