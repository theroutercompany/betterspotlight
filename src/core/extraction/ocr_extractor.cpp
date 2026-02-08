#include "core/extraction/ocr_extractor.h"
#include "core/shared/logging.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>

#ifdef TESSERACT_FOUND
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#endif

namespace bs {

namespace {

const QSet<QString>& ocrSupportedExtensions()
{
    static const QSet<QString> exts = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("webp"),
        QStringLiteral("bmp"),
        QStringLiteral("tiff"),
        QStringLiteral("tif"),
    };
    return exts;
}

} // anonymous namespace

// ── Impl (pimpl) ────────────────────────────────────────────

struct OcrExtractor::Impl {
#ifdef TESSERACT_FOUND
    std::unique_ptr<tesseract::TessBaseAPI> api;
    bool initialised = false;

    Impl()
    {
        api = std::make_unique<tesseract::TessBaseAPI>();
        // Initialise with English; nullptr = default tessdata path
        int rc = api->Init(nullptr, "eng");
        if (rc != 0) {
            LOG_ERROR(bsExtraction, "Tesseract Init failed (rc=%d). "
                      "Check TESSDATA_PREFIX and eng.traineddata presence.", rc);
            api.reset();
            initialised = false;
        } else {
            initialised = true;
            LOG_INFO(bsExtraction, "Tesseract OCR engine initialised (lang=eng)");
        }
    }

    ~Impl()
    {
        if (api) {
            api->End();
        }
    }
#else
    bool initialised = false;
#endif
};

// ── Construction / destruction ──────────────────────────────

OcrExtractor::OcrExtractor()
    : m_impl(std::make_unique<Impl>())
{
}

OcrExtractor::~OcrExtractor() = default;

OcrExtractor::OcrExtractor(OcrExtractor&&) noexcept = default;
OcrExtractor& OcrExtractor::operator=(OcrExtractor&&) noexcept = default;

// ── Interface ───────────────────────────────────────────────

bool OcrExtractor::supports(const QString& extension) const
{
    return ocrSupportedExtensions().contains(extension.toLower());
}

ExtractionResult OcrExtractor::extract(const QString& filePath)
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

#ifdef TESSERACT_FOUND
    if (!m_impl || !m_impl->initialised || !m_impl->api) {
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = QStringLiteral("Tesseract engine failed to initialise");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    // Load image via Leptonica
    const QByteArray pathUtf8 = filePath.toUtf8();
    Pix* image = pixRead(pathUtf8.constData());
    if (!image) {
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = QStringLiteral("Leptonica failed to read image");
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_WARN(bsExtraction, "Leptonica pixRead failed: %s", qUtf8Printable(filePath));
        return result;
    }

    // Convert to grayscale if needed (improves OCR accuracy)
    Pix* gray = nullptr;
    if (pixGetDepth(image) > 8) {
        gray = pixConvertRGBToGray(image, 0.0f, 0.0f, 0.0f);  // equal weight
        pixDestroy(&image);
        if (!gray) {
            result.status = ExtractionResult::Status::CorruptedFile;
            result.errorMessage = QStringLiteral("Failed to convert image to grayscale");
            result.durationMs = static_cast<int>(timer.elapsed());
            return result;
        }
        image = gray;
        gray = nullptr;
    }

    // Set image for Tesseract
    m_impl->api->SetImage(image);

    // Perform OCR
    char* ocrText = m_impl->api->GetUTF8Text();
    if (ocrText) {
        result.status = ExtractionResult::Status::Success;
        result.content = QString::fromUtf8(ocrText);
        delete[] ocrText;
    } else {
        result.status = ExtractionResult::Status::Success;
        result.content = QString();
        LOG_DEBUG(bsExtraction, "OCR produced no text for: %s", qUtf8Printable(filePath));
    }

    // Clean up Tesseract state and Leptonica image
    m_impl->api->Clear();
    pixDestroy(&image);

    result.durationMs = static_cast<int>(timer.elapsed());
    LOG_DEBUG(bsExtraction, "OCR extracted %lld chars from %s in %d ms",
              static_cast<long long>(result.content.value_or(QString()).size()),
              qUtf8Printable(filePath),
              result.durationMs);

    return result;

#else
    // Tesseract not available
    result.status = ExtractionResult::Status::UnsupportedFormat;
    result.errorMessage = QStringLiteral("OCR extraction unavailable (Tesseract not found)");
    result.durationMs = static_cast<int>(timer.elapsed());
    LOG_INFO(bsExtraction, "OCR extraction skipped (no Tesseract): %s",
             qUtf8Printable(filePath));
    return result;
#endif
}

} // namespace bs
