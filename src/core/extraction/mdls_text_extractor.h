#pragma once

#include "core/extraction/extractor.h"

namespace bs {

class MdlsTextExtractor : public FileExtractor {
public:
    ExtractionResult extract(const QString& filePath) override;
    bool supports(const QString& extension) const override;
};

} // namespace bs
