#include <QDebug>
#include <QFile>

#include <cmath>

#include "core/embedding/embedding_manager.h"

#if defined(ONNXRUNTIME_FOUND) && __has_include(<onnxruntime_cxx_api.h>)
#define BS_WITH_ONNX 1
#include <dlfcn.h>
#include <onnxruntime_cxx_api.h>
#else
#define BS_WITH_ONNX 0
#endif

namespace bs {

namespace {

const QString kBgeQueryPrefix = QStringLiteral(
    "Represent this sentence for searching relevant passages: ");

#if BS_WITH_ONNX
constexpr uint32_t kCoreMlFlagUseCpuAndGpu = 0U;

#ifdef ORT_COREML_FLAG_CREATE_MLPROGRAM
constexpr uint32_t kCoreMlFlagCreateMlProgram = ORT_COREML_FLAG_CREATE_MLPROGRAM;
#elif defined(COREML_FLAG_CREATE_MLPROGRAM)
constexpr uint32_t kCoreMlFlagCreateMlProgram = COREML_FLAG_CREATE_MLPROGRAM;
#else
constexpr uint32_t kCoreMlFlagCreateMlProgram = 0U;
#endif

using AppendCoreMlProviderFn = OrtStatus* (*)(OrtSessionOptions* options, uint32_t coremlFlags);
#endif

#if BS_WITH_ONNX
Ort::Env& ortEnvironment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "betterspotlight-embedding");
    return env;
}
#endif

} // anonymous namespace

class EmbeddingManager::Impl {
public:
#if BS_WITH_ONNX
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string outputName;
#endif
};

EmbeddingManager::EmbeddingManager(const QString& modelPath, const QString& vocabPath)
    : m_impl(std::make_unique<Impl>())
    , m_modelPath(modelPath)
    , m_tokenizer(vocabPath)
{
}

EmbeddingManager::~EmbeddingManager() = default;

bool EmbeddingManager::initialize()
{
#if BS_WITH_ONNX
    if (!m_tokenizer.isLoaded()) {
        qWarning() << "EmbeddingManager initialize failed: tokenizer vocab not loaded";
        m_available = false;
        return false;
    }

    if (m_modelPath.isEmpty() || !QFile::exists(m_modelPath)) {
        qWarning() << "EmbeddingManager initialize failed: model file missing at" << m_modelPath;
        m_available = false;
        return false;
    }

    try {
        m_impl->sessionOptions.SetIntraOpNumThreads(2);
        m_impl->sessionOptions.SetInterOpNumThreads(1);
        m_impl->sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        m_impl->sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const uint32_t coremlFlags = kCoreMlFlagUseCpuAndGpu | kCoreMlFlagCreateMlProgram;
        AppendCoreMlProviderFn appendCoreMlProvider = reinterpret_cast<AppendCoreMlProviderFn>(
            dlsym(RTLD_DEFAULT, "OrtSessionOptionsAppendExecutionProvider_CoreML"));
        if (appendCoreMlProvider != nullptr) {
            OrtSessionOptions* rawOptions = m_impl->sessionOptions;
            OrtStatus* coremlStatus = appendCoreMlProvider(rawOptions, coremlFlags);
            if (coremlStatus != nullptr) {
                const OrtApi& api = Ort::GetApi();
                qWarning() << "CoreML EP unavailable, falling back to CPU:"
                           << api.GetErrorMessage(coremlStatus);
                api.ReleaseStatus(coremlStatus);
            }
        } else {
            qWarning() << "CoreML EP symbol not found, using CPU provider";
        }

        m_impl->session = std::make_unique<Ort::Session>(
            ortEnvironment(), m_modelPath.toUtf8().constData(), m_impl->sessionOptions);

        const size_t inputCount = m_impl->session->GetInputCount();
        const size_t outputCount = m_impl->session->GetOutputCount();
        if (inputCount < 3 || outputCount < 1) {
            qWarning() << "EmbeddingManager initialize failed: unexpected model IO counts"
                       << inputCount << outputCount;
            m_impl->session.reset();
            m_available = false;
            return false;
        }

        Ort::AllocatorWithDefaultOptions allocator;
        bool sawInputIds = false;
        bool sawAttentionMask = false;
        bool sawTokenTypeIds = false;

        for (size_t i = 0; i < inputCount; ++i) {
            Ort::AllocatedStringPtr inputName = m_impl->session->GetInputNameAllocated(i, allocator);
            const std::string name = inputName.get() ? inputName.get() : "";
            sawInputIds = sawInputIds || (name == "input_ids");
            sawAttentionMask = sawAttentionMask || (name == "attention_mask");
            sawTokenTypeIds = sawTokenTypeIds || (name == "token_type_ids");
        }

        if (!sawInputIds || !sawAttentionMask || !sawTokenTypeIds) {
            qWarning() << "EmbeddingManager initialize failed: required inputs missing";
            m_impl->session.reset();
            m_available = false;
            return false;
        }

        Ort::AllocatedStringPtr outputName = m_impl->session->GetOutputNameAllocated(0, allocator);
        if (!outputName || outputName.get()[0] == '\0') {
            qWarning() << "EmbeddingManager initialize failed: model output name missing";
            m_impl->session.reset();
            m_available = false;
            return false;
        }
        m_impl->outputName = outputName.get();

        m_available = true;
        return true;
    } catch (const Ort::Exception& ex) {
        qWarning() << "EmbeddingManager initialize failed:" << ex.what();
    }

    m_available = false;
    return false;
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
    return embed(kBgeQueryPrefix + text);
}

std::vector<std::vector<float>> EmbeddingManager::embedBatch(const std::vector<QString>& texts)
{
#if BS_WITH_ONNX
    if (!m_available || !m_impl->session || texts.empty()) {
        return {};
    }

    const BatchTokenizerOutput tokenized = m_tokenizer.tokenizeBatch(texts);
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

        if (shape.size() == 2 && shape[0] == tokenized.batchSize && shape[1] == kEmbeddingSize) {
            for (int i = 0; i < tokenized.batchSize; ++i) {
                std::vector<float> embedding(static_cast<size_t>(kEmbeddingSize));
                const float* row = data + static_cast<size_t>(i * kEmbeddingSize);
                for (int j = 0; j < kEmbeddingSize; ++j) {
                    embedding[static_cast<size_t>(j)] = row[static_cast<size_t>(j)];
                }
                embeddings.push_back(normalizeEmbedding(std::move(embedding)));
            }
            return embeddings;
        }

        if (shape.size() == 3 && shape[0] == tokenized.batchSize
            && shape[2] == kEmbeddingSize && shape[1] >= 1) {
            const int64_t seqLen = shape[1];
            const int64_t hidden = shape[2];
            for (int i = 0; i < tokenized.batchSize; ++i) {
                std::vector<float> embedding(static_cast<size_t>(kEmbeddingSize));
                const float* cls = data + static_cast<size_t>(i * seqLen * hidden);
                for (int j = 0; j < kEmbeddingSize; ++j) {
                    embedding[static_cast<size_t>(j)] = cls[static_cast<size_t>(j)];
                }
                embeddings.push_back(normalizeEmbedding(std::move(embedding)));
            }
            return embeddings;
        }

        qWarning() << "EmbeddingManager inference failed: unsupported output shape";
        return {};
    } catch (const Ort::Exception& ex) {
        qWarning() << "EmbeddingManager inference failed:" << ex.what();
        return {};
    }
#else
    Q_UNUSED(texts);
    return {};
#endif
}

} // namespace bs
