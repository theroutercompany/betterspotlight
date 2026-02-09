#pragma once

#include "core/extraction/extractor.h"
#include "core/extraction/mdls_text_extractor.h"
#include "core/extraction/text_extractor.h"
#include "core/extraction/pdf_extractor.h"
#include "core/extraction/ocr_extractor.h"
#include "core/shared/types.h"

#include <QSemaphore>
#include <atomic>
#include <mutex>
#include <memory>

namespace bs {

// ExtractionManager — coordinates content extraction across file types.
//
// Routes files to the appropriate extractor based on ItemKind, enforces
// a concurrency limit via QSemaphore, and provides configurable size and
// timeout thresholds.
//
// Usage:
//   ExtractionManager mgr;
//   ExtractionResult r = mgr.extract("/path/to/file.py", ItemKind::Code);
//
// Thread safety: multiple threads may call extract() concurrently.
// The semaphore limits the number of in-flight extractions.
class ExtractionManager {
public:
    ExtractionManager();
    ~ExtractionManager();

    // Non-copyable, non-movable (owns semaphore state)
    ExtractionManager(const ExtractionManager&) = delete;
    ExtractionManager& operator=(const ExtractionManager&) = delete;
    ExtractionManager(ExtractionManager&&) = delete;
    ExtractionManager& operator=(ExtractionManager&&) = delete;

    // Extract content from a file, selecting the right extractor
    // based on its ItemKind. Returns immediately for non-extractable
    // kinds (Directory, Archive, Binary, Unknown) with no content.
    ExtractionResult extract(const QString& filePath, ItemKind kind);

    // ── Configuration ───────────────────────────────────────

    // Maximum concurrent extractions (default 4).
    // Takes effect on the next extract() call.
    void setMaxConcurrent(int max);

    // Per-extraction timeout in milliseconds (default 30000).
    // When exceeded, the extraction is abandoned and Timeout is returned.
    void setTimeoutMs(int timeoutMs);

    // Maximum file size in bytes (default 50 MB).
    // Files exceeding this are rejected with SizeExceeded.
    void setMaxFileSizeBytes(int64_t sz);

    // Request cancellation of any in-progress or upcoming extraction.
    void requestCancel();

    // Clear the cancellation flag (call before starting a new batch).
    void clearCancel();

    // Check if cancellation has been requested.
    bool isCancelRequested() const;

    // Maximum time for a single extraction before returning partial results.
    static constexpr int kMaxExtractionMs = 30000;

private:
    std::unique_ptr<MdlsTextExtractor> m_mdlsTextExtractor;
    std::unique_ptr<TextExtractor> m_textExtractor;
    std::unique_ptr<PdfExtractor> m_pdfExtractor;
    std::unique_ptr<OcrExtractor> m_ocrExtractor;

    int m_maxConcurrent = 4;
    int m_timeoutMs = 30000;
    int64_t m_maxFileSize = 50 * 1024 * 1024;

    std::atomic<bool> m_cancelRequested{false};

    QSemaphore m_concurrencySemaphore{4};
    QSemaphore m_pdfSemaphore{1};
    QSemaphore m_ocrSemaphore{1};
    std::mutex m_ocrMutex;

    // Select the appropriate extractor for a given ItemKind.
    // Returns nullptr for non-extractable kinds.
    FileExtractor* selectExtractor(ItemKind kind) const;
};

} // namespace bs
