#include "core/extraction/extraction_manager.h"
#include "core/extraction/text_cleaner.h"
#include "core/shared/logging.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

namespace bs {

namespace {

constexpr qint64 kTextProbeBytes = 8192;
constexpr double kMaxSuspiciousByteRatio = 0.02;

bool looksLikeTextPayload(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return true;
    }

    int suspicious = 0;
    for (unsigned char b : bytes) {
        if (b == 0) {
            return false;
        }
        if (b < 0x09) {
            ++suspicious;
            continue;
        }
        if (b >= 0x0E && b < 0x20) {
            ++suspicious;
        }
    }

    return (static_cast<double>(suspicious) / static_cast<double>(bytes.size()))
           <= kMaxSuspiciousByteRatio;
}

bool shouldFallbackToTextByProbe(const QFileInfo& info, const QString& filePath)
{
    QFile probe(filePath);
    if (!probe.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray sample = probe.read(kTextProbeBytes);
    probe.close();

    if (sample.isEmpty() && info.size() > 0) {
        return false;
    }
    return looksLikeTextPayload(sample);
}

} // namespace

// ── Construction / destruction ──────────────────────────────

ExtractionManager::ExtractionManager()
    : m_mdlsTextExtractor(std::make_unique<MdlsTextExtractor>())
    , m_textExtractor(std::make_unique<TextExtractor>())
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

void ExtractionManager::requestCancel()
{
    m_cancelRequested.store(true);
    LOG_INFO(bsExtraction, "Extraction cancellation requested");
}

void ExtractionManager::clearCancel()
{
    m_cancelRequested.store(false);
}

bool ExtractionManager::isCancelRequested() const
{
    return m_cancelRequested.load();
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
        return nullptr;
    case ItemKind::Unknown:
        // Unknown extension files still get a text probe fallback.
        return m_textExtractor.get();
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

        // Honor extractor-specific extension support to avoid routing formats
        // (e.g. .icns) into extractors that cannot decode them.
        const QString extension = info.suffix().toLower();

        if ((kind == ItemKind::Text || kind == ItemKind::Code || kind == ItemKind::Markdown)
            && m_mdlsTextExtractor
            && m_mdlsTextExtractor->supports(extension)) {
            extractor = m_mdlsTextExtractor.get();
        }

        if (!extractor->supports(extension)) {
            const bool textKind = (kind == ItemKind::Text
                                   || kind == ItemKind::Code
                                   || kind == ItemKind::Markdown
                                   || kind == ItemKind::Unknown);
            const bool canProbeFallback = (extractor == m_textExtractor.get()) && textKind;
            if (canProbeFallback && shouldFallbackToTextByProbe(info, filePath)) {
                LOG_INFO(bsExtraction,
                         "Text fallback enabled for unknown extension '%s': %s",
                         qUtf8Printable(extension.isEmpty()
                                            ? QStringLiteral("<none>")
                                            : extension),
                         qUtf8Printable(filePath));
            } else {
                result.status = ExtractionResult::Status::UnsupportedFormat;
                result.errorMessage = QString("Extension '%1' is not supported by extractor")
                                          .arg(extension.isEmpty()
                                               ? QStringLiteral("<none>")
                                               : extension);
                result.durationMs = 0;
                return result;
            }
        }
    }

    // Check for cancellation before acquiring semaphore
    if (m_cancelRequested.load()) {
        result.status = ExtractionResult::Status::Cancelled;
        result.errorMessage = QStringLiteral("Extraction was cancelled");
        result.durationMs = 0;
        return result;
    }

    // Acquire semaphore permit with timeout
    if (!m_concurrencySemaphore.tryAcquire(1, m_timeoutMs)) {
        result.status = ExtractionResult::Status::Timeout;
        result.errorMessage = QStringLiteral("Timed out waiting for extraction slot");
        result.durationMs = m_timeoutMs;
        LOG_WARN(bsExtraction, "Extraction slot timeout for: %s", qUtf8Printable(filePath));
        return result;
    }

    QSemaphore* heavySemaphore = nullptr;
    if (kind == ItemKind::Pdf) {
        heavySemaphore = &m_pdfSemaphore;
    } else if (kind == ItemKind::Image) {
        heavySemaphore = &m_ocrSemaphore;
    }

    if (heavySemaphore && !heavySemaphore->tryAcquire(1, m_timeoutMs)) {
        m_concurrencySemaphore.release();
        result.status = ExtractionResult::Status::Timeout;
        result.errorMessage = QStringLiteral("Timed out waiting for extractor kind slot");
        result.durationMs = m_timeoutMs;
        LOG_WARN(bsExtraction, "Extractor kind slot timeout for: %s", qUtf8Printable(filePath));
        return result;
    }

    // Perform the extraction within the semaphore-guarded section
    QElapsedTimer timer;
    timer.start();

    LOG_DEBUG(bsExtraction, "Starting extraction: %s (kind=%s)",
              qUtf8Printable(filePath),
              qUtf8Printable(itemKindToString(kind)));

    if (kind == ItemKind::Image) {
        // Tesseract's API object is mutable and not safe for concurrent calls.
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        result = extractor->extract(filePath);
    } else {
        result = extractor->extract(filePath);
    }

    // Override duration to include semaphore wait time
    result.durationMs = static_cast<int>(timer.elapsed());

    // Enforce per-extraction deadline — if extraction took too long, flag it
    if (result.durationMs > kMaxExtractionMs) {
        LOG_WARN(bsExtraction, "Extraction exceeded deadline (%d ms > %d ms): %s",
                 result.durationMs, kMaxExtractionMs, qUtf8Printable(filePath));
        if (result.status == ExtractionResult::Status::Success) {
            // Keep partial results but note the timeout in the log
            LOG_INFO(bsExtraction, "Extraction completed past deadline, returning partial result: %s",
                     qUtf8Printable(filePath));
        }
    }

    if (heavySemaphore) {
        heavySemaphore->release();
    }
    m_concurrencySemaphore.release();

    if (result.status == ExtractionResult::Status::Success && result.content.has_value()) {
        result.content = TextCleaner::clean(result.content.value());
    }

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
