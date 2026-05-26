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
#include <utility>

#if defined(ONNXRUNTIME_FOUND) && __has_include(<onnxruntime_cxx_api.h>)
#define BS_WITH_ONNX 1
#include <onnxruntime_cxx_api.h>
#else
#define BS_WITH_ONNX 0
#endif

namespace bs {

namespace cross_encoder_detail {

std::optional<std::vector<size_t>> candidateLogitOffsets(const std::vector<int64_t>& shape,
                                                         int candidateCount,
                                                         size_t elementCount)
{
    if (candidateCount <= 0 || elementCount == 0 || shape.empty()) {
        return std::nullopt;
    }

    std::vector<size_t> offsets;
    offsets.reserve(static_cast<size_t>(candidateCount));

    if (shape.size() == 1) {
        if (shape[0] < candidateCount) {
            return std::nullopt;
        }
        for (int i = 0; i < candidateCount; ++i) {
            const size_t offset = static_cast<size_t>(i);
            if (offset >= elementCount) {
                return std::nullopt;
            }
            offsets.push_back(offset);
        }
        return offsets;
    }

    if (shape.size() == 2) {
        const int64_t rows = shape[0];
        const int64_t cols = shape[1];
        if (rows != candidateCount || cols <= 0) {
            return std::nullopt;
        }

        const int64_t scoreColumn = cols > 1 ? 1 : 0;
        for (int i = 0; i < candidateCount; ++i) {
            const int64_t rawOffset = static_cast<int64_t>(i) * cols + scoreColumn;
            if (rawOffset < 0 || static_cast<uint64_t>(rawOffset) >= elementCount) {
                return std::nullopt;
            }
            offsets.push_back(static_cast<size_t>(rawOffset));
        }
        return offsets;
    }

    return std::nullopt;
}

std::optional<float> sigmoidScoreFromLogit(float logit)
{
    if (!std::isfinite(logit)) {
        return std::nullopt;
    }

    const double scaled = std::clamp(static_cast<double>(logit), -60.0, 60.0);
    const double score = 1.0 / (1.0 + std::exp(-scaled));
    if (!std::isfinite(score)) {
        return std::nullopt;
    }
    return static_cast<float>(std::clamp(score, 0.0, 1.0));
}

} // namespace cross_encoder_detail

namespace {

#if BS_WITH_ONNX
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
#endif

} // namespace

class CrossEncoderReranker::Impl {
public:
#if BS_WITH_ONNX
    Ort::Session* session = nullptr;  // Borrowed from ModelSession
    std::vector<std::string> inputNames;
    std::string outputName;
#endif
    std::unique_ptr<WordPieceTokenizer> tokenizer;
    ModelRegistry* registry = nullptr;
    bool available = false;
};

CrossEncoderReranker::CrossEncoderReranker(ModelRegistry* registry, std::string role)
    : m_impl(std::make_unique<Impl>())
    , m_role(std::move(role))
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

    if (m_role.empty()) {
        m_role = "cross-encoder";
    }
    ModelSession* modelSession = m_impl->registry->getSession(m_role);
    if (!modelSession || !modelSession->isAvailable()) {
        qWarning() << "CrossEncoderReranker:" << QString::fromStdString(m_role)
                   << "session unavailable";
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
    m_impl->inputNames.clear();
    for (const QString& name : entry.inputs) {
        if (!name.isEmpty()) {
            m_impl->inputNames.push_back(name.toStdString());
        }
    }
    if (m_impl->inputNames.empty()) {
        m_impl->inputNames = {"input_ids", "attention_mask", "token_type_ids"};
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
        std::vector<Ort::Value> inputTensors;
        std::vector<const char*> inputNamePtrs;
        inputTensors.reserve(m_impl->inputNames.size());
        inputNamePtrs.reserve(m_impl->inputNames.size());

        for (const std::string& inputName : m_impl->inputNames) {
            if (inputName == "input_ids") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(batch.inputIds.data()),
                    batch.inputIds.size(),
                    inputShape, 2));
            } else if (inputName == "attention_mask") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(batch.attentionMask.data()),
                    batch.attentionMask.size(),
                    inputShape, 2));
            } else if (inputName == "token_type_ids") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(batch.tokenTypeIds.data()),
                    batch.tokenTypeIds.size(),
                    inputShape, 2));
            } else {
                qWarning() << "CrossEncoderReranker manifest input unsupported:"
                           << QString::fromStdString(inputName);
                return 0;
            }
            inputNamePtrs.push_back(inputName.c_str());
        }

        const char* outputNames[1] = {m_impl->outputName.c_str()};

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            inputNamePtrs.data(), inputTensors.data(), inputTensors.size(),
            outputNames, 1);

        if (outputs.empty() || !outputs[0].IsTensor()) {
            qWarning() << "CrossEncoderReranker: missing tensor output";
            return 0;
        }

        const Ort::TensorTypeAndShapeInfo outputInfo = outputs[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> outputShape = outputInfo.GetShape();
        const size_t outputElementCount = outputInfo.GetElementCount();
        const auto logitOffsets =
            cross_encoder_detail::candidateLogitOffsets(outputShape,
                                                        candidateCount,
                                                        outputElementCount);
        if (!logitOffsets.has_value()) {
            qWarning() << "CrossEncoderReranker: unsupported logits shape for candidate count"
                       << candidateCount;
            return 0;
        }

        const float* logits = outputs[0].GetTensorData<float>();
        if (!logits) {
            return 0;
        }

        int boostedCount = 0;
        for (int i = 0; i < candidateCount; ++i) {
            const float logit = logits[logitOffsets->at(static_cast<size_t>(i))];
            const std::optional<float> sigmoid =
                cross_encoder_detail::sigmoidScoreFromLogit(logit);
            if (!sigmoid.has_value()) {
                qWarning() << "CrossEncoderReranker: non-finite logit for candidate" << i;
                continue;
            }

            auto& result = results[static_cast<size_t>(i)];
            result.crossEncoderScore = *sigmoid;

            if (*sigmoid >= config.minScoreThreshold) {
                const double boost = static_cast<double>(config.weight)
                    * static_cast<double>(*sigmoid);
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
