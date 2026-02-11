#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bs {

class ModelRegistry;
class WordPieceTokenizer;

struct EmbeddingCircuitBreaker {
    std::atomic<int> consecutiveFailures{0};
    std::atomic<int64_t> lastFailureTime{0};
    static constexpr int kOpenThreshold = 5;          // Open after 5 consecutive failures
    static constexpr int kHalfOpenDelayMs = 30000;    // Try again after 30s

    bool isOpen() const;
    void recordSuccess();
    void recordFailure();
};

class EmbeddingManager {
public:
    explicit EmbeddingManager(ModelRegistry* registry,
                              std::string role = "bi-encoder");
    ~EmbeddingManager();

    EmbeddingManager(const EmbeddingManager&) = delete;
    EmbeddingManager& operator=(const EmbeddingManager&) = delete;
    EmbeddingManager(EmbeddingManager&&) = delete;
    EmbeddingManager& operator=(EmbeddingManager&&) = delete;

    bool initialize();
    bool isAvailable() const;
    int embeddingDimensions() const;
    QString activeModelId() const;
    QString activeGenerationId() const;
    QString providerName() const;
    QString semanticAggregationMode() const;

    std::vector<float> embed(const QString& text);
    std::vector<float> embedQuery(const QString& text);
    std::vector<std::vector<float>> embedBatch(const std::vector<QString>& texts);

    // Expose for testing
    EmbeddingCircuitBreaker& circuitBreaker() { return m_circuitBreaker; }

private:
    std::vector<float> normalizeEmbedding(std::vector<float> embedding) const;

    class Impl;
    std::unique_ptr<Impl> m_impl;

    ModelRegistry* m_registry = nullptr;
    std::string m_role;
    std::unique_ptr<WordPieceTokenizer> m_tokenizer;
    int m_embeddingSize = 0;
    QString m_queryPrefix;
    QString m_activeModelId;
    QString m_activeGenerationId;
    QString m_providerName;
    QString m_semanticAggregationMode;
    bool m_available = false;
    EmbeddingCircuitBreaker m_circuitBreaker;
};

} // namespace bs
