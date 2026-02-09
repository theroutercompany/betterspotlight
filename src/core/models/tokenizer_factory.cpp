#include "core/models/tokenizer_factory.h"

#include "core/shared/logging.h"

#include <QFile>

namespace bs {

std::unique_ptr<WordPieceTokenizer> TokenizerFactory::create(const ModelManifestEntry& entry,
                                                              const QString& modelsDir)
{
    if (entry.tokenizer != QStringLiteral("wordpiece")) {
        LOG_WARN(bsCore, "TokenizerFactory: unsupported tokenizer type '%s'",
                 qPrintable(entry.tokenizer));
        return nullptr;
    }

    if (entry.vocab.isEmpty()) {
        LOG_WARN(bsCore, "TokenizerFactory: no vocab file specified for model '%s'",
                 qPrintable(entry.name));
        return nullptr;
    }

    const QString vocabPath = modelsDir + QStringLiteral("/") + entry.vocab;
    if (!QFile::exists(vocabPath)) {
        LOG_WARN(bsCore, "TokenizerFactory: vocab file not found at %s", qPrintable(vocabPath));
        return nullptr;
    }

    auto tokenizer = std::make_unique<WordPieceTokenizer>(vocabPath);
    if (!tokenizer->isLoaded()) {
        LOG_WARN(bsCore, "TokenizerFactory: failed to load vocab from %s", qPrintable(vocabPath));
        return nullptr;
    }

    return tokenizer;
}

} // namespace bs
