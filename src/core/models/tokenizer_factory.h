#pragma once

#include "core/embedding/tokenizer.h"
#include "core/models/model_manifest.h"

#include <QString>

#include <memory>

namespace bs {

class TokenizerFactory {
public:
    // Creates a tokenizer for the given manifest entry.
    // Currently only supports "wordpiece" tokenizer type.
    // Returns nullptr if the tokenizer type is unsupported or the vocab file is missing.
    static std::unique_ptr<WordPieceTokenizer> create(const ModelManifestEntry& entry,
                                                      const QString& modelsDir);
};

} // namespace bs
