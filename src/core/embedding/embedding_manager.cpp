#include "core/embedding/embedding_manager.h"
#include "core/embedding/tokenizer.h"
#include "core/models/model_registry.h"
#include "core/models/model_session.h"
#include "core/models/model_manifest.h"
#include "core/models/tokenizer_factory.h"

#include <QDebug>
#include <QElapsedTimer>

#include <chrono>
#include <cmath>

#if defined(ONNXRUNTIME_FOUND) && __has_include(<onnxruntime_cxx_api.h>)
#define BS_WITH_ONNX 1
#include <onnxruntime_cxx_api.h>
#else
#define BS_WITH_ONNX 0
#endif

namespace bs {

bool EmbeddingCircuitBreaker::isOpen() const
{
    if (consecutiveFailures.load() < kOpenThreshold) {
        return false;
    }
    // In open state — check if enough time has elapsed for half-open
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const int64_t lastFail = lastFailureTime.load();
    if (now - lastFail >= kHalfOpenDelayMs) {
        return false;  // half-open: allow one attempt
    }
    return true;
}

void EmbeddingCircuitBreaker::recordSuccess()
{
    consecutiveFailures.store(0);
}

void EmbeddingCircuitBreaker::recordFailure()
{
    consecutiveFailures.fetch_add(1);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    lastFailureTime.store(now);
}

class EmbeddingManager::Impl {
public:
#if BS_WITH_ONNX
    Ort::Session* session = nullptr;  // Borrowed from ModelSession — NOT owned
    std::string outputName;
#endif
};

EmbeddingManager::EmbeddingManager(ModelRegistry* registry)
    : m_impl(std::make_unique<Impl>())
    , m_registry(registry)
{
}

EmbeddingManager::~EmbeddingManager() = default;

bool EmbeddingManager::initialize()
{
#if BS_WITH_ONNX
    if (!m_registry) {
        qWarning() << "EmbeddingManager initialize failed: null registry";
        m_available = false;
        return false;
    }

    ModelSession* modelSession = m_registry->getSession("bi-encoder");
    if (!modelSession || !modelSession->isAvailable()) {
        qWarning() << "EmbeddingManager initialize failed: bi-encoder session unavailable";
        m_available = false;
        return false;
    }

    const ModelManifestEntry& entry = modelSession->manifest();

    m_tokenizer = TokenizerFactory::create(entry, m_registry->modelsDir());
    if (!m_tokenizer || !m_tokenizer->isLoaded()) {
        qWarning() << "EmbeddingManager initialize failed: tokenizer creation failed";
        m_available = false;
        return false;
    }

    m_embeddingSize = entry.dimensions;
    if (m_embeddingSize <= 0) {
        qWarning() << "EmbeddingManager initialize failed: invalid dimensions" << m_embeddingSize;
        m_available = false;
        return false;
    }

    m_queryPrefix = entry.queryPrefix;

    m_impl->session = static_cast<Ort::Session*>(modelSession->rawSession());
    if (!m_impl->session) {
        qWarning() << "EmbeddingManager initialize failed: null ONNX session";
        m_available = false;
        return false;
    }

    const auto& outputNames = modelSession->outputNames();
    if (outputNames.empty()) {
        qWarning() << "EmbeddingManager initialize failed: no output names";
        m_available = false;
        return false;
    }
    m_impl->outputName = outputNames.front();

    m_available = true;
    return true;
#else
    qWarning() << "EmbeddingManager initialize skipped: ONNX Runtime not enabled";
    m_available = false;
    return false;
#endif
}

bool EmbeddingManager::isAvailable() const
{
    return m_available;
}

std::vector<float> EmbeddingManager::normalizeEmbedding(std::vector<float> embedding) const
{
    double sumSquares = 0.0;
    for (const float value : embedding) {
        sumSquares += static_cast<double>(value) * static_cast<double>(value);
    }

    const double norm = std::sqrt(sumSquares);
    if (norm <= 0.0) {
        return embedding;
    }

    for (float& value : embedding) {
        value = static_cast<float>(static_cast<double>(value) / norm);
    }
    return embedding;
}

std::vector<float> EmbeddingManager::embed(const QString& text)
{
    const std::vector<std::vector<float>> result = embedBatch({text});
    if (result.empty()) {
        return {};
    }
    return result.front();
}

std::vector<float> EmbeddingManager::embedQuery(const QString& text)
{
    return embed(m_queryPrefix + text);
}

std::vector<std::vector<float>> EmbeddingManager::embedBatch(const std::vector<QString>& texts)
{
#if BS_WITH_ONNX
    if (!m_available || !m_impl->session || !m_tokenizer || texts.empty()) {
        return {};
    }

    if (m_circuitBreaker.isOpen()) {
        qWarning() << "EmbeddingManager circuit breaker is open, skipping inference";
        return {};
    }

    const BatchTokenizerOutput tokenized = m_tokenizer->tokenizeBatch(texts);
    if (tokenized.batchSize <= 0 || tokenized.seqLength <= 0) {
        return {};
    }

    const int64_t inputShape[2] = {
        static_cast<int64_t>(tokenized.batchSize),
        static_cast<int64_t>(tokenized.seqLength),
    };

    try {
        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator,
                                                                 OrtMemTypeDefault);

        Ort::Value inputIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(tokenized.inputIds.data()),
            tokenized.inputIds.size(),
            inputShape,
            2);

        Ort::Value attentionMask = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(tokenized.attentionMask.data()),
            tokenized.attentionMask.size(),
            inputShape,
            2);

        Ort::Value tokenTypeIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(tokenized.tokenTypeIds.data()),
            tokenized.tokenTypeIds.size(),
            inputShape,
            2);

        Ort::Value inputTensors[3] = {
            std::move(inputIds),
            std::move(attentionMask),
            std::move(tokenTypeIds),
        };

        static constexpr const char* inputNames[3] = {
            "input_ids",
            "attention_mask",
            "token_type_ids",
        };
        const char* outputNames[1] = {m_impl->outputName.c_str()};

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            inputNames,
            inputTensors,
            3,
            outputNames,
            1);

        if (outputs.empty() || !outputs[0].IsTensor()) {
            qWarning() << "EmbeddingManager inference failed: missing tensor output";
            return {};
        }

        Ort::TensorTypeAndShapeInfo info = outputs[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> shape = info.GetShape();
        const float* data = outputs[0].GetTensorData<float>();
        if (!data) {
            return {};
        }

        std::vector<std::vector<float>> embeddings;
        embeddings.reserve(static_cast<size_t>(tokenized.batchSize));

        if (shape.size() == 2 && shape[0] == tokenized.batchSize && shape[1] == m_embeddingSize) {
            for (int i = 0; i < tokenized.batchSize; ++i) {
                std::vector<float> embedding(static_cast<size_t>(m_embeddingSize));
                const float* row = data + static_cast<size_t>(i * m_embeddingSize);
                for (int j = 0; j < m_embeddingSize; ++j) {
                    embedding[static_cast<size_t>(j)] = row[static_cast<size_t>(j)];
                }
                embeddings.push_back(normalizeEmbedding(std::move(embedding)));
            }
            m_circuitBreaker.recordSuccess();
            return embeddings;
        }

        if (shape.size() == 3 && shape[0] == tokenized.batchSize
            && shape[2] == m_embeddingSize && shape[1] >= 1) {
            const int64_t seqLen = shape[1];
            const int64_t hidden = shape[2];
            for (int i = 0; i < tokenized.batchSize; ++i) {
                std::vector<float> embedding(static_cast<size_t>(m_embeddingSize));
                const float* cls = data + static_cast<size_t>(i * seqLen * hidden);
                for (int j = 0; j < m_embeddingSize; ++j) {
                    embedding[static_cast<size_t>(j)] = cls[static_cast<size_t>(j)];
                }
                embeddings.push_back(normalizeEmbedding(std::move(embedding)));
            }
            m_circuitBreaker.recordSuccess();
            return embeddings;
        }

        qWarning() << "EmbeddingManager inference failed: unsupported output shape";
        m_circuitBreaker.recordFailure();
        return {};
    } catch (const Ort::Exception& ex) {
        qWarning() << "EmbeddingManager inference failed:" << ex.what();
        m_circuitBreaker.recordFailure();
        return {};
    }
#else
    Q_UNUSED(texts);
    return {};
#endif
}

} // namespace bs
