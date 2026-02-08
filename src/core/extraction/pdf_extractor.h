#pragma once

#include "core/extraction/extractor.h"

namespace bs {

// PdfExtractor — extracts text from PDF files using Poppler.
//
// When compiled with BS_POPPLER_QT6 or BS_POPPLER_CPP, uses Poppler to
// iterate pages and extract text. Without Poppler, returns UnsupportedFormat.
//
// Limits:
//   - 1000-page cap per document
//   - 10 MB extracted text cap
//   - Encrypted PDFs are rejected (CorruptedFile status)
class PdfExtractor : public FileExtractor {
public:
    ExtractionResult extract(const QString& filePath) override;
    bool supports(const QString& extension) const override;
};

} // namespace bs
