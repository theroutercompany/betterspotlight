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

// ── Public entry points ─────────────────────────────────────

PreparedWork Indexer::prepareWorkItem(const WorkItem& item, uint64_t generation)
{
    switch (item.type) {
    case WorkItem::Type::Delete:
        return prepareDelete(item, generation);
    case WorkItem::Type::ModifiedContent:
    case WorkItem::Type::NewFile:
        return prepareNewOrModified(item, generation);
    case WorkItem::Type::RescanDirectory:
        return prepareRescan(item, generation);
    }

    PreparedWork prepared;
    prepared.type = item.type;
    prepared.path = QString::fromStdString(item.filePath);
    prepared.generation = generation;
    prepared.retryCount = item.retryCount;
    prepared.validation = ValidationResult::Exclude;
    return prepared;
}

IndexResult Indexer::applyPreparedWork(const PreparedWork& prepared)
{
    switch (prepared.type) {
    case WorkItem::Type::Delete:
        return applyDelete(prepared);
    case WorkItem::Type::ModifiedContent:
    case WorkItem::Type::NewFile:
        return applyNewOrModified(prepared);
    case WorkItem::Type::RescanDirectory:
        return applyRescan(prepared);
    }

    IndexResult result;
    result.status = IndexResult::Status::Excluded;
    return result;
}

IndexResult Indexer::processWorkItem(const WorkItem& item)
{
    PreparedWork prepared = prepareWorkItem(item, 0);
    return applyPreparedWork(prepared);
}

// ── Prep stage ──────────────────────────────────────────────

PreparedWork Indexer::prepareDelete(const WorkItem& item, uint64_t generation)
{
    PreparedWork prepared;
    prepared.type = WorkItem::Type::Delete;
    prepared.path = QString::fromStdString(item.filePath);
    prepared.generation = generation;
    prepared.retryCount = item.retryCount;
    prepared.validation = ValidationResult::Include;
    return prepared;
}

PreparedWork Indexer::prepareNewOrModified(const WorkItem& item, uint64_t generation)
{
    QElapsedTimer timer;
    timer.start();

    PreparedWork prepared;
    prepared.type = item.type;
    prepared.path = QString::fromStdString(item.filePath);
    prepared.generation = generation;
    prepared.retryCount = item.retryCount;

    auto validation = m_pathRules.validate(item.filePath);
    prepared.validation = validation;

    if (validation == ValidationResult::Exclude) {
        prepared.prepDurationMs = static_cast<int>(timer.elapsed());
        return prepared;
    }

    auto meta = extractMetadata(item.filePath);
    if (!meta.has_value()) {
        prepared.failure = PreparedFailure{
            QStringLiteral("metadata"),
            QStringLiteral("Cannot stat or access file"),
            std::nullopt
        };
        prepared.prepDurationMs = static_cast<int>(timer.elapsed());
        return prepared;
    }

    prepared.metadata = meta;
    prepared.sensitivity = m_pathRules.classifySensitivity(item.filePath);

    QFileInfo fi(prepared.path);
    prepared.parentPath = fi.path();

    if (validation == ValidationResult::MetadataOnly) {
        prepared.prepDurationMs = static_cast<int>(timer.elapsed());
        return prepared;
    }

    if (meta->itemKind == ItemKind::Directory
        || meta->itemKind == ItemKind::Archive
        || meta->itemKind == ItemKind::Binary
        || meta->itemKind == ItemKind::Unknown) {
        prepared.nonExtractable = true;
        prepared.prepDurationMs = static_cast<int>(timer.elapsed());
        return prepared;
    }

    prepareExtractedContent(prepared, meta.value(), item.retryCount);

    prepared.prepDurationMs = static_cast<int>(timer.elapsed());
    return prepared;
}

PreparedWork Indexer::prepareRescan(const WorkItem& item, uint64_t generation)
{
    QElapsedTimer timer;
    timer.start();

    PreparedWork prepared;
    prepared.type = WorkItem::Type::RescanDirectory;
    prepared.path = QString::fromStdString(item.filePath);
    prepared.generation = generation;
    prepared.retryCount = item.retryCount;

    auto validation = m_pathRules.validate(item.filePath);
    prepared.validation = validation;
    if (validation == ValidationResult::Exclude) {
        prepared.prepDurationMs = static_cast<int>(timer.elapsed());
        return prepared;
    }

    auto meta = extractMetadata(item.filePath);
    if (!meta.has_value()) {
        prepared.failure = PreparedFailure{
            QStringLiteral("metadata"),
            QStringLiteral("Cannot stat or access file"),
            std::nullopt
        };
        prepared.prepDurationMs = static_cast<int>(timer.elapsed());
        return prepared;
    }

    prepared.metadata = meta;
    prepared.sensitivity = m_pathRules.classifySensitivity(item.filePath);

    QFileInfo fi(prepared.path);
    prepared.parentPath = fi.path();
    prepared.prepDurationMs = static_cast<int>(timer.elapsed());
    return prepared;
}

void Indexer::prepareExtractedContent(PreparedWork& prepared,
                                      const FileMetadata& meta,
                                      int initialRetryCount)
{
    static constexpr int kMaxRetries = 2;

    int attempts = initialRetryCount;
    ExtractionResult extraction;

    do {
        extraction = m_extractor.extract(prepared.path, meta.itemKind);
        if (extraction.status == ExtractionResult::Status::Success
            && extraction.content.has_value()) {
            break;
        }

        ++attempts;
        if (attempts <= kMaxRetries) {
            LOG_WARN(bsIndex, "Retrying extraction (%d/%d): %s",
                     attempts, kMaxRetries, meta.filePath.c_str());
        }
    } while (attempts <= kMaxRetries);

    if (extraction.status != ExtractionResult::Status::Success
        || !extraction.content.has_value()) {
        if (extraction.status == ExtractionResult::Status::UnsupportedFormat) {
            // Optional extractor backend not available (e.g., Poppler/Tesseract):
            // keep metadata indexed without recording a hard failure.
            prepared.nonExtractable = true;
            return;
        }

        prepared.failure = PreparedFailure{
            QStringLiteral("extraction"),
            extraction.errorMessage.value_or(
                QStringLiteral("Extraction failed with no details")),
            extraction.status
        };
        return;
    }

    const QString& content = extraction.content.value();
    const QByteArray contentUtf8 = content.toUtf8();

    prepared.contentHash = QString::fromLatin1(
        QCryptographicHash::hash(contentUtf8, QCryptographicHash::Sha256).toHex());
    prepared.chunks = m_chunker.chunkContent(prepared.path, content);
    prepared.hasExtractedContent = true;
}

// ── Writer stage ────────────────────────────────────────────

IndexResult Indexer::applyDelete(const PreparedWork& prepared)
{
    QElapsedTimer timer;
    timer.start();

    IndexResult result;
    result.status = IndexResult::Status::Deleted;

    auto existing = m_store.getItemByPath(prepared.path);
    if (existing.has_value()) {
        m_store.deleteChunksForItem(existing->id, prepared.path);
        m_store.deleteItemByPath(prepared.path);
        LOG_INFO(bsIndex, "Deleted from index: %s (id=%lld)",
                 qUtf8Printable(prepared.path),
                 static_cast<long long>(existing->id));
    } else {
        LOG_DEBUG(bsIndex, "Delete requested for non-indexed path: %s",
                  qUtf8Printable(prepared.path));
    }

    result.durationMs = static_cast<int>(timer.elapsed());
    return result;
}

IndexResult Indexer::applyNewOrModified(const PreparedWork& prepared)
{
    QElapsedTimer timer;
    timer.start();

    IndexResult result;

    if (prepared.validation == ValidationResult::Exclude) {
        result.status = IndexResult::Status::Excluded;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!prepared.metadata.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    const FileMetadata& meta = prepared.metadata.value();

    auto existing = m_store.getItemByPath(prepared.path);
    if (existing.has_value() && prepared.type == WorkItem::Type::ModifiedContent) {
        if (static_cast<int64_t>(meta.fileSize) == existing->size
            && meta.modifiedAt == existing->modifiedAt) {
            result.status = IndexResult::Status::Skipped;
            result.durationMs = static_cast<int>(timer.elapsed());
            return result;
        }
    }

    const QString sensitivityStr = sensitivityToString(prepared.sensitivity);
    const QString existingHash = existing.has_value() ? existing->contentHash : QString();

    auto itemId = m_store.upsertItem(
        prepared.path,
        QString::fromStdString(meta.fileName),
        QString::fromStdString(meta.extension),
        meta.itemKind,
        static_cast<int64_t>(meta.fileSize),
        meta.createdAt,
        meta.modifiedAt,
        existingHash,
        sensitivityStr,
        prepared.parentPath);

    if (!itemId.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (prepared.validation == ValidationResult::MetadataOnly) {
        result.status = IndexResult::Status::MetadataOnly;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (prepared.nonExtractable) {
        m_store.clearFailures(itemId.value());
        result.status = IndexResult::Status::Indexed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (prepared.failure.has_value()) {
        m_store.recordFailure(itemId.value(),
                              prepared.failure->stage,
                              prepared.failure->message);
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!prepared.hasExtractedContent) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!existingHash.isEmpty() && existingHash == prepared.contentHash) {
        result.status = IndexResult::Status::Skipped;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (prepared.chunks.empty()) {
        m_store.updateContentHash(itemId.value(), prepared.contentHash);
        m_store.clearFailures(itemId.value());
        result.status = IndexResult::Status::Indexed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    const bool insertOk = m_store.insertChunks(itemId.value(),
                                               QString::fromStdString(meta.fileName),
                                               prepared.path,
                                               prepared.chunks);
    if (!insertOk) {
        m_store.recordFailure(itemId.value(),
                              QStringLiteral("fts5_insert"),
                              QStringLiteral("insertChunks() returned false"));
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    m_store.updateContentHash(itemId.value(), prepared.contentHash);
    m_store.clearFailures(itemId.value());

    result.status = IndexResult::Status::Indexed;
    result.chunksInserted = static_cast<int>(prepared.chunks.size());
    result.durationMs = static_cast<int>(timer.elapsed());
    return result;
}

IndexResult Indexer::applyRescan(const PreparedWork& prepared)
{
    QElapsedTimer timer;
    timer.start();

    IndexResult result;

    if (prepared.validation == ValidationResult::Exclude) {
        result.status = IndexResult::Status::Excluded;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!prepared.metadata.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    const FileMetadata& meta = prepared.metadata.value();
    const QString sensitivityStr = sensitivityToString(prepared.sensitivity);
    const auto existing = m_store.getItemByPath(prepared.path);
    const QString existingHash = existing.has_value() ? existing->contentHash : QString();

    auto itemId = m_store.upsertItem(
        prepared.path,
        QString::fromStdString(meta.fileName),
        QString::fromStdString(meta.extension),
        meta.itemKind,
        static_cast<int64_t>(meta.fileSize),
        meta.createdAt,
        meta.modifiedAt,
        existingHash,
        sensitivityStr,
        prepared.parentPath);

    if (!itemId.has_value()) {
        result.status = IndexResult::Status::ExtractionFailed;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

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

    if (S_ISDIR(st.st_mode)) {
        meta.itemKind = ItemKind::Directory;
    } else {
        meta.itemKind = FileScanner::classifyItemKind(meta.extension, st.st_mode);
    }

    return meta;
}

} // namespace bs
