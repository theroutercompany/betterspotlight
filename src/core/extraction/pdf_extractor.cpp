#include "core/extraction/pdf_extractor.h"
#include "core/shared/logging.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#if defined(BS_POPPLER_QT6)
#include <poppler/qt6/poppler-qt6.h>
#elif defined(BS_POPPLER_CPP)
#include <poppler-document.h>
#include <poppler-page.h>
#endif

namespace bs {

namespace {

bool looksLikeOfflinePlaceholder(const QFileInfo& info, const QString& filePath)
{
    if (info.size() <= 0) {
        return false;
    }

    QFile probe(filePath);
    if (!probe.open(QIODevice::ReadOnly)) {
        return true;
    }

    const QByteArray sample = probe.read(4096);
    probe.close();
    return sample.isEmpty();
}

} // namespace

bool PdfExtractor::supports(const QString& extension) const
{
    return extension.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

ExtractionResult PdfExtractor::extract(const QString& filePath)
{
    QElapsedTimer timer;
    timer.start();

    ExtractionResult result;

    // Check file accessibility
    QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        result.status = ExtractionResult::Status::Inaccessible;
        result.errorMessage = QStringLiteral("File does not exist or is not a regular file");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!info.isReadable()) {
        result.status = ExtractionResult::Status::Inaccessible;
        result.errorMessage = QStringLiteral("File is not readable");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (looksLikeOfflinePlaceholder(info, filePath)) {
        LOG_INFO(bsExtraction, "PDF placeholder detected before parser load: %s",
                 qUtf8Printable(filePath));
        result.status = ExtractionResult::Status::Inaccessible;
        result.errorMessage =
            QStringLiteral("File appears to be a cloud placeholder (size reported but no content readable)");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

#if defined(BS_POPPLER_QT6) || defined(BS_POPPLER_CPP)
    constexpr int kMaxPages = 1000;
    constexpr int64_t kMaxExtractedTextBytes = 10LL * 1024 * 1024;

#if defined(BS_POPPLER_QT6)
    // Load the PDF document
    std::unique_ptr<Poppler::Document> doc(Poppler::Document::load(filePath));
#elif defined(BS_POPPLER_CPP)
    std::unique_ptr<poppler::document> doc(
        poppler::document::load_from_file(filePath.toStdString()));
#endif
    if (!doc) {
        result.status = ExtractionResult::Status::CorruptedFile;
        result.errorMessage = QStringLiteral("Failed to load PDF document");
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_WARN(bsExtraction, "Poppler failed to load: %s", qUtf8Printable(filePath));
        return result;
    }

    // Reject encrypted/locked PDFs
#if defined(BS_POPPLER_QT6)
    if (doc->isLocked()) {
#elif defined(BS_POPPLER_CPP)
    if (doc->is_locked()) {
#endif
        result.status = ExtractionResult::Status::CorruptedFile;
        result.errorMessage = QStringLiteral("PDF is encrypted or password-protected");
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_INFO(bsExtraction, "Skipping encrypted PDF: %s", qUtf8Printable(filePath));
        return result;
    }

    const int pageCount =
#if defined(BS_POPPLER_QT6)
        doc->numPages();
#elif defined(BS_POPPLER_CPP)
        doc->pages();
#endif
    const int pagesToProcess = std::min(pageCount, kMaxPages);

    if (pageCount > kMaxPages) {
        LOG_INFO(bsExtraction, "PDF has %d pages, capping at %d: %s",
                 pageCount, kMaxPages, qUtf8Printable(filePath));
    }

    QString fullText;
    fullText.reserve(4096);  // reasonable starting estimate

    for (int i = 0; i < pagesToProcess; ++i) {
#if defined(BS_POPPLER_QT6)
        std::unique_ptr<Poppler::Page> page(doc->page(i));
#elif defined(BS_POPPLER_CPP)
        std::unique_ptr<poppler::page> page(doc->create_page(i));
#endif
        if (!page) {
            LOG_DEBUG(bsExtraction, "Null page %d in %s", i, qUtf8Printable(filePath));
            continue;
        }

        // Page separator
        if (i > 0) {
            fullText += QStringLiteral("\n");
        }
        fullText += QString("--- Page %1 ---\n").arg(i + 1);

#if defined(BS_POPPLER_QT6)
        QString pageText = page->text(QRectF());
#elif defined(BS_POPPLER_CPP)
        const poppler::byte_array utf8Page = page->text().to_utf8();
        QString pageText = QString::fromUtf8(utf8Page.data(),
                                             static_cast<int>(utf8Page.size()));
#endif
        if (!pageText.isEmpty()) {
            fullText += pageText;
        }

        // Check extracted text size limit
        if (fullText.toUtf8().size() > kMaxExtractedTextBytes) {
            LOG_INFO(bsExtraction, "Extracted text exceeded %lld bytes at page %d: %s",
                     static_cast<long long>(kMaxExtractedTextBytes), i + 1,
                     qUtf8Printable(filePath));
            fullText += QStringLiteral("\n[... truncated due to size limit ...]");
            break;
        }
    }

    result.status = ExtractionResult::Status::Success;
    result.content = std::move(fullText);
    result.durationMs = static_cast<int>(timer.elapsed());

    LOG_DEBUG(bsExtraction, "Extracted %d pages from PDF %s in %d ms",
              pagesToProcess, qUtf8Printable(filePath), result.durationMs);

    return result;

#else
    // Poppler not available — cannot extract PDF content
    result.status = ExtractionResult::Status::UnsupportedFormat;
    result.errorMessage = QStringLiteral("PDF extraction unavailable (Poppler not found)");
    result.durationMs = static_cast<int>(timer.elapsed());
    LOG_INFO(bsExtraction, "PDF extraction skipped (no Poppler): %s",
             qUtf8Printable(filePath));
    return result;
#endif
}

} // namespace bs
