#pragma once

#include "core/embedding/tokenizer.h"

#include <QString>

#include <memory>
#include <vector>

namespace bs {

class EmbeddingManager {
public:
    EmbeddingManager(const QString& modelPath, const QString& vocabPath);
    ~EmbeddingManager();

    EmbeddingManager(const EmbeddingManager&) = delete;
    EmbeddingManager& operator=(const EmbeddingManager&) = delete;
    EmbeddingManager(EmbeddingManager&&) = delete;
    EmbeddingManager& operator=(EmbeddingManager&&) = delete;

    bool initialize();
    bool isAvailable() const;

    std::vector<float> embed(const QString& text);
    std::vector<float> embedQuery(const QString& text);
    std::vector<std::vector<float>> embedBatch(const std::vector<QString>& texts);

private:
    static constexpr int kEmbeddingSize = 384;

    std::vector<float> normalizeEmbedding(std::vector<float> embedding) const;

    class Impl;
    std::unique_ptr<Impl> m_impl;

    QString m_modelPath;
    WordPieceTokenizer m_tokenizer;
    bool m_available = false;
};

} // namespace bs
