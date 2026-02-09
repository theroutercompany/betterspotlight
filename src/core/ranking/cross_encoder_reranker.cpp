#include "core/ranking/cross_encoder_reranker.h"
#include "core/embedding/tokenizer.h"
#include "core/models/model_registry.h"
#include "core/models/model_session.h"
#include "core/models/model_manifest.h"
#include "core/models/tokenizer_factory.h"

#include <QDebug>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

#if defined(ONNXRUNTIME_FOUND) && __has_include(<onnxruntime_cxx_api.h>)
#define BS_WITH_ONNX 1
#include <onnxruntime_cxx_api.h>
#else
#define BS_WITH_ONNX 0
#endif

namespace bs {

namespace {

QString buildDocumentText(const SearchResult& result)
{
    // "name | parentPath | plainSnippet"
    QString parentPath = QFileInfo(result.path).absolutePath();
    QString plainSnippet = result.snippet;
    plainSnippet.replace(QStringLiteral("<b>"), QString());
    plainSnippet.replace(QStringLiteral("</b>"), QString());

    QString doc = result.name;
    if (!parentPath.isEmpty()) {
        doc += QStringLiteral(" | ") + parentPath;
    }
    if (!plainSnippet.isEmpty()) {
        doc += QStringLiteral(" | ") + plainSnippet;
    }
    return doc;
}

} // namespace

class CrossEncoderReranker::Impl {
public:
#if BS_WITH_ONNX
    Ort::Session* session = nullptr;  // Borrowed from ModelSession
    std::string outputName;
#endif
    std::unique_ptr<WordPieceTokenizer> tokenizer;
    ModelRegistry* registry = nullptr;
    bool available = false;
};

CrossEncoderReranker::CrossEncoderReranker(ModelRegistry* registry)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->registry = registry;
}

CrossEncoderReranker::~CrossEncoderReranker() = default;

bool CrossEncoderReranker::initialize()
{
#if BS_WITH_ONNX
    if (!m_impl->registry) {
        qWarning() << "CrossEncoderReranker: null registry";
        return false;
    }

    ModelSession* modelSession = m_impl->registry->getSession("cross-encoder");
    if (!modelSession || !modelSession->isAvailable()) {
        qWarning() << "CrossEncoderReranker: cross-encoder session unavailable";
        return false;
    }

    const ModelManifestEntry& entry = modelSession->manifest();

    m_impl->tokenizer = TokenizerFactory::create(entry, m_impl->registry->modelsDir());
    if (!m_impl->tokenizer || !m_impl->tokenizer->isLoaded()) {
        qWarning() << "CrossEncoderReranker: tokenizer creation failed";
        return false;
    }

    m_impl->session = static_cast<Ort::Session*>(modelSession->rawSession());
    if (!m_impl->session) {
        qWarning() << "CrossEncoderReranker: null ONNX session";
        return false;
    }

    const auto& outputNames = modelSession->outputNames();
    if (outputNames.empty()) {
        qWarning() << "CrossEncoderReranker: no output names";
        return false;
    }
    m_impl->outputName = outputNames.front();

    m_impl->available = true;
    return true;
#else
    qWarning() << "CrossEncoderReranker: ONNX Runtime not enabled";
    return false;
#endif
}

bool CrossEncoderReranker::isAvailable() const
{
    return m_impl->available;
}

int CrossEncoderReranker::rerank(const QString& query,
                                  std::vector<SearchResult>& results,
                                  const RerankerConfig& config) const
{
#if BS_WITH_ONNX
    if (!m_impl->available || !m_impl->session || !m_impl->tokenizer || results.empty()) {
        return 0;
    }

    const int candidateCount = std::min(static_cast<int>(results.size()), config.maxCandidates);
    if (candidateCount <= 0) {
        return 0;
    }

    // Build (query, documentText) pairs for top-N candidates
    std::vector<std::pair<QString, QString>> pairs;
    pairs.reserve(static_cast<size_t>(candidateCount));
    for (int i = 0; i < candidateCount; ++i) {
        pairs.emplace_back(query, buildDocumentText(results[static_cast<size_t>(i)]));
    }

    // Batch tokenize
    const auto batch = m_impl->tokenizer->tokenizePairBatch(pairs);
    if (batch.batchSize <= 0 || batch.sequenceLength <= 0) {
        return 0;
    }

    try {
        const int64_t inputShape[2] = {
            static_cast<int64_t>(batch.batchSize),
            static_cast<int64_t>(batch.sequenceLength),
        };

        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
            OrtAllocatorType::OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(batch.inputIds.data()),
            batch.inputIds.size(),
            inputShape, 2);

        Ort::Value attentionMask = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(batch.attentionMask.data()),
            batch.attentionMask.size(),
            inputShape, 2);

        Ort::Value tokenTypeIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(batch.tokenTypeIds.data()),
            batch.tokenTypeIds.size(),
            inputShape, 2);

        Ort::Value inputTensors[3] = {
            std::move(inputIds),
            std::move(attentionMask),
            std::move(tokenTypeIds),
        };

        static constexpr const char* inputNames[3] = {
            "input_ids", "attention_mask", "token_type_ids",
        };
        const char* outputNames[1] = {m_impl->outputName.c_str()};

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            inputNames, inputTensors, 3,
            outputNames, 1);

        if (outputs.empty() || !outputs[0].IsTensor()) {
            qWarning() << "CrossEncoderReranker: missing tensor output";
            return 0;
        }

        const float* logits = outputs[0].GetTensorData<float>();
        if (!logits) {
            return 0;
        }

        int boostedCount = 0;
        for (int i = 0; i < candidateCount; ++i) {
            const float logit = logits[i];
            const float sigmoid = 1.0f / (1.0f + std::exp(-logit));

            auto& result = results[static_cast<size_t>(i)];
            result.crossEncoderScore = sigmoid;

            if (sigmoid >= config.minScoreThreshold) {
                const double boost = static_cast<double>(config.weight) * static_cast<double>(sigmoid);
                result.score += boost;
                result.scoreBreakdown.crossEncoderBoost = boost;
                ++boostedCount;
            }
        }

        return boostedCount;
    } catch (const Ort::Exception& ex) {
        qWarning() << "CrossEncoderReranker inference failed:" << ex.what();
        return 0;
    }
#else
    Q_UNUSED(query);
    Q_UNUSED(results);
    Q_UNUSED(config);
    return 0;
#endif
}

} // namespace bs
