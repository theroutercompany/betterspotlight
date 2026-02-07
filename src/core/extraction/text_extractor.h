#pragma once

#include "core/extraction/extractor.h"
#include <QSet>

namespace bs {

// TextExtractor — reads plain-text and source-code files.
//
// Handles 100+ file extensions covering programming languages, markup,
// configuration files, and data formats. Attempts UTF-8 decoding first,
// falling back to Latin-1 for binary-safe conversion.
//
// Size limit: files larger than 50 MB are rejected with SizeExceeded.
class TextExtractor : public FileExtractor {
public:
    ExtractionResult extract(const QString& filePath) override;
    bool supports(const QString& extension) const override;

private:
    static const QSet<QString>& supportedExtensions();
};

} // namespace bs
