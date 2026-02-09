#pragma once

#include <QString>

#include <memory>
#include <vector>

namespace bs {

class ModelRegistry;
class WordPieceTokenizer;

class EmbeddingManager {
public:
    explicit EmbeddingManager(ModelRegistry* registry);
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
    std::vector<float> normalizeEmbedding(std::vector<float> embedding) const;

    class Impl;
    std::unique_ptr<Impl> m_impl;

    ModelRegistry* m_registry = nullptr;
    std::unique_ptr<WordPieceTokenizer> m_tokenizer;
    int m_embeddingSize = 0;
    QString m_queryPrefix;
    bool m_available = false;
};

} // namespace bs
