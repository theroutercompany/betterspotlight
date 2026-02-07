#include "core/extraction/extraction_manager.h"
#include "core/shared/logging.h"

#include <QElapsedTimer>
#include <QFileInfo>

namespace bs {

// ── Construction / destruction ──────────────────────────────

ExtractionManager::ExtractionManager()
    : m_textExtractor(std::make_unique<TextExtractor>())
    , m_pdfExtractor(std::make_unique<PdfExtractor>())
    , m_ocrExtractor(std::make_unique<OcrExtractor>())
{
    LOG_INFO(bsExtraction, "ExtractionManager initialised (concurrency=%d, timeout=%d ms, maxSize=%lld)",
             m_maxConcurrent, m_timeoutMs, static_cast<long long>(m_maxFileSize));
}

ExtractionManager::~ExtractionManager() = default;

// ── Configuration ───────────────────────────────────────────

void ExtractionManager::setMaxConcurrent(int max)
{
    if (max < 1) {
        LOG_WARN(bsExtraction, "setMaxConcurrent(%d) clamped to 1", max);
        max = 1;
    }

    // Adjust semaphore capacity. The difference between old and new
    // capacity is released (if increasing) or acquired (if decreasing).
    int delta = max - m_maxConcurrent;
    if (delta > 0) {
        m_concurrencySemaphore.release(delta);
    } else if (delta < 0) {
        // Attempt to acquire the excess permits; non-blocking best-effort.
        // If permits are in use, the effective concurrency will decrease
        // as in-flight extractions complete.
        m_concurrencySemaphore.tryAcquire(-delta);
    }

    m_maxConcurrent = max;
    LOG_INFO(bsExtraction, "Max concurrent extractions set to %d", m_maxConcurrent);
}

void ExtractionManager::setTimeoutMs(int timeoutMs)
{
    if (timeoutMs < 0) {
        LOG_WARN(bsExtraction, "setTimeoutMs(%d) clamped to 0", timeoutMs);
        timeoutMs = 0;
    }
    m_timeoutMs = timeoutMs;
    LOG_INFO(bsExtraction, "Extraction timeout set to %d ms", m_timeoutMs);
}

void ExtractionManager::setMaxFileSizeBytes(int64_t sz)
{
    if (sz < 0) {
        LOG_WARN(bsExtraction, "setMaxFileSizeBytes(%lld) clamped to 0",
                 static_cast<long long>(sz));
        sz = 0;
    }
    m_maxFileSize = sz;
    LOG_INFO(bsExtraction, "Max file size set to %lld bytes",
             static_cast<long long>(m_maxFileSize));
}

// ── Extractor selection ─────────────────────────────────────

FileExtractor* ExtractionManager::selectExtractor(ItemKind kind) const
{
    switch (kind) {
    case ItemKind::Text:
    case ItemKind::Code:
    case ItemKind::Markdown:
        return m_textExtractor.get();

    case ItemKind::Pdf:
        return m_pdfExtractor.get();

    case ItemKind::Image:
        return m_ocrExtractor.get();

    case ItemKind::Directory:
    case ItemKind::Archive:
    case ItemKind::Binary:
    case ItemKind::Unknown:
        return nullptr;
    }

    return nullptr;
}

// ── Main extraction entry point ─────────────────────────────

ExtractionResult ExtractionManager::extract(const QString& filePath, ItemKind kind)
{
    ExtractionResult result;

    // Non-extractable kinds return immediately
    FileExtractor* extractor = selectExtractor(kind);
    if (!extractor) {
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = QString("ItemKind '%1' is not extractable")
                                  .arg(itemKindToString(kind));
        result.durationMs = 0;
        return result;
    }

    // Pre-flight file size check against the configurable limit
    {
        QFileInfo info(filePath);
        if (!info.exists() || !info.isFile()) {
            result.status = ExtractionResult::Status::Inaccessible;
            result.errorMessage = QStringLiteral("File does not exist or is not a regular file");
            result.durationMs = 0;
            return result;
        }

        if (info.size() > m_maxFileSize) {
            result.status = ExtractionResult::Status::SizeExceeded;
            result.errorMessage = QString("File size %1 exceeds configured limit %2")
                                      .arg(info.size())
                                      .arg(m_maxFileSize);
            LOG_INFO(bsExtraction, "Skipping oversized file: %s (%lld bytes, limit %lld)",
                     qUtf8Printable(filePath),
                     static_cast<long long>(info.size()),
                     static_cast<long long>(m_maxFileSize));
            return result;
        }
    }

    // Acquire semaphore permit with timeout
    if (!m_concurrencySemaphore.tryAcquire(1, m_timeoutMs)) {
        result.status = ExtractionResult::Status::Timeout;
        result.errorMessage = QStringLiteral("Timed out waiting for extraction slot");
        result.durationMs = m_timeoutMs;
        LOG_WARN(bsExtraction, "Extraction slot timeout for: %s", qUtf8Printable(filePath));
        return result;
    }

    // Perform the extraction within the semaphore-guarded section
    QElapsedTimer timer;
    timer.start();

    LOG_DEBUG(bsExtraction, "Starting extraction: %s (kind=%s)",
              qUtf8Printable(filePath),
              qUtf8Printable(itemKindToString(kind)));

    result = extractor->extract(filePath);

    // Override duration to include semaphore wait time
    result.durationMs = static_cast<int>(timer.elapsed());

    m_concurrencySemaphore.release();

    if (result.status == ExtractionResult::Status::Success) {
        LOG_DEBUG(bsExtraction, "Extraction succeeded: %s (%d ms, %lld chars)",
                  qUtf8Printable(filePath),
                  result.durationMs,
                  static_cast<long long>(result.content.value_or(QString()).size()));
    } else {
        LOG_INFO(bsExtraction, "Extraction failed: %s (status=%d, %s)",
                 qUtf8Printable(filePath),
                 static_cast<int>(result.status),
                 qUtf8Printable(result.errorMessage.value_or(QStringLiteral("no details"))));
    }

    return result;
}

} // namespace bs
