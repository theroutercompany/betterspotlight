#pragma once

#include "core/extraction/extractor.h"
#include <memory>

namespace bs {

// OcrExtractor — extracts text from images via Tesseract OCR.
//
// When compiled with TESSERACT_FOUND, initialises a Tesseract engine
// (English language model) and uses Leptonica for image I/O and
// grayscale conversion. Without Tesseract, returns UnsupportedFormat.
//
// Supported image formats: PNG, JPEG, WebP, BMP, TIFF.
class OcrExtractor : public FileExtractor {
public:
    OcrExtractor();
    ~OcrExtractor() override;

    // Non-copyable (owns Tesseract engine state)
    OcrExtractor(const OcrExtractor&) = delete;
    OcrExtractor& operator=(const OcrExtractor&) = delete;
    OcrExtractor(OcrExtractor&&) noexcept;
    OcrExtractor& operator=(OcrExtractor&&) noexcept;

    ExtractionResult extract(const QString& filePath) override;
    bool supports(const QString& extension) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bs
