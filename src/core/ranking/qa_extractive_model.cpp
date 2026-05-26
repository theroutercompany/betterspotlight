#include "core/ranking/qa_extractive_model.h"

#include "core/embedding/tokenizer.h"
#include "core/models/model_manifest.h"
#include "core/models/model_registry.h"
#include "core/models/model_session.h"
#include "core/models/tokenizer_factory.h"

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#if defined(ONNXRUNTIME_FOUND) && __has_include(<onnxruntime_cxx_api.h>)
#define BS_WITH_ONNX 1
#include <onnxruntime_cxx_api.h>
#else
#define BS_WITH_ONNX 0
#endif

namespace bs {

namespace qa_extractive_detail {

double confidenceForRawScore(double rawScore)
{
    if (!std::isfinite(rawScore)) {
        return 0.0;
    }

    const double scaled = std::clamp(rawScore / 6.0, -60.0, 60.0);
    return std::clamp(1.0 / (1.0 + std::exp(-scaled)), 0.0, 1.0);
}

SpanSelection selectBestSpan(const float* startLogits,
                             const float* endLogits,
                             int contextStart,
                             int contextEnd,
                             int maxSpanTokens)
{
    SpanSelection out;
    if (!startLogits || !endLogits || contextStart < 0 || contextEnd < contextStart
        || maxSpanTokens <= 0) {
        return out;
    }

    double bestScore = -std::numeric_limits<double>::infinity();
    int bestStart = -1;
    int bestEnd = -1;

    for (int s = contextStart; s <= contextEnd; ++s) {
        const double startScore = static_cast<double>(startLogits[s]);
        if (!std::isfinite(startScore)) {
            continue;
        }

        const int maxEnd = std::min(contextEnd, s + maxSpanTokens - 1);
        for (int e = s; e <= maxEnd; ++e) {
            const double endScore = static_cast<double>(endLogits[e]);
            if (!std::isfinite(endScore)) {
                continue;
            }

            const double score = startScore + endScore;
            if (!std::isfinite(score)) {
                continue;
            }
            if (score > bestScore) {
                bestScore = score;
                bestStart = s;
                bestEnd = e;
            }
        }
    }

    if (bestStart < 0 || bestEnd < bestStart) {
        return out;
    }

    out.available = true;
    out.startToken = bestStart;
    out.endToken = bestEnd;
    out.rawScore = bestScore;
    out.confidence = confidenceForRawScore(bestScore);
    return out;
}

OutputNameSelection selectOutputNames(const std::vector<std::string>& outputNames,
                                      bool allowSingleOutputFallback)
{
    OutputNameSelection out;
    if (outputNames.empty()) {
        return out;
    }

    const auto itStart = std::find_if(outputNames.begin(), outputNames.end(),
                                      [](const std::string& value) {
                                          return value.find("start") != std::string::npos;
                                      });
    const auto itEnd = std::find_if(outputNames.begin(), outputNames.end(),
                                    [](const std::string& value) {
                                        return value.find("end") != std::string::npos;
                                    });
    if (itStart != outputNames.end() && itEnd != outputNames.end() && itStart != itEnd) {
        out.available = true;
        out.startOutputName = *itStart;
        out.endOutputName = *itEnd;
        return out;
    }

    if (outputNames.size() >= 2) {
        out.available = true;
        out.startOutputName = outputNames[0];
        out.endOutputName = outputNames[1];
        return out;
    }

    if (allowSingleOutputFallback) {
        out.available = true;
        out.startOutputName = outputNames.front();
        out.endOutputName = outputNames.front();
    }
    return out;
}

} // namespace qa_extractive_detail

namespace {

#if BS_WITH_ONNX
bool envFlagEnabled(const QString& raw)
{
    const QString normalized = raw.trimmed().toLower();
    return normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on");
}

QString normalizeAnswerText(const QString& text, int maxChars)
{
    QString normalized = text.simplified();
    if (normalized.size() <= maxChars) {
        return normalized;
    }
    return normalized.left(std::max(0, maxChars - 3)).trimmed() + QStringLiteral("...");
}

bool isSentenceBoundary(QChar c)
{
    return c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?')
        || c == QLatin1Char('\n') || c == QLatin1Char('\r');
}

QString extractSentenceAround(const QString& context, int centerChar, int maxChars)
{
    if (context.trimmed().isEmpty()) {
        return QString();
    }

    const int len = context.size();
    if (len <= maxChars) {
        return context.simplified();
    }

    centerChar = std::clamp(centerChar, 0, std::max(0, len - 1));
    int left = centerChar;
    int right = centerChar;
    while (left > 0 && !isSentenceBoundary(context.at(left - 1))) {
        --left;
    }
    while (right + 1 < len && !isSentenceBoundary(context.at(right + 1))) {
        ++right;
    }

    QString sentence = context.mid(left, right - left + 1).simplified();
    if (sentence.isEmpty()) {
        const int span = std::min(maxChars, len);
        const int start = std::clamp(centerChar - (span / 2), 0, std::max(0, len - span));
        sentence = context.mid(start, span).simplified();
    }
    return normalizeAnswerText(sentence, maxChars);
}
#endif

} // namespace

class QaExtractiveModel::Impl {
public:
#if BS_WITH_ONNX
    Ort::Session* session = nullptr; // Borrowed from ModelSession
    std::vector<std::string> inputNames;
    std::string startOutputName;
    std::string endOutputName;
#endif
    std::unique_ptr<WordPieceTokenizer> tokenizer;
    ModelRegistry* registry = nullptr;
    bool available = false;
};

QaExtractiveModel::QaExtractiveModel(ModelRegistry* registry, std::string role)
    : m_impl(std::make_unique<Impl>())
    , m_role(std::move(role))
{
    m_impl->registry = registry;
}

QaExtractiveModel::~QaExtractiveModel() = default;

bool QaExtractiveModel::initialize()
{
#if BS_WITH_ONNX
    if (!m_impl->registry) {
        return false;
    }
    if (m_role.empty()) {
        m_role = "qa-extractive";
    }

    ModelSession* modelSession = m_impl->registry->getSession(m_role);
    if (!modelSession || !modelSession->isAvailable()) {
        return false;
    }

    const ModelManifestEntry& entry = modelSession->manifest();
    m_impl->tokenizer = TokenizerFactory::create(entry, m_impl->registry->modelsDir());
    if (!m_impl->tokenizer || !m_impl->tokenizer->isLoaded()) {
        return false;
    }

    m_impl->session = static_cast<Ort::Session*>(modelSession->rawSession());
    if (!m_impl->session) {
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
    const bool allowSingleOutputFallback =
        qEnvironmentVariableIsSet("BS_TEST_QA_SINGLE_OUTPUT_FALLBACK")
        && envFlagEnabled(qEnvironmentVariable("BS_TEST_QA_SINGLE_OUTPUT_FALLBACK"));
    const qa_extractive_detail::OutputNameSelection outputSelection =
        qa_extractive_detail::selectOutputNames(outputNames, allowSingleOutputFallback);
    if (!outputSelection.available) {
        return false;
    }
    m_impl->startOutputName = outputSelection.startOutputName;
    m_impl->endOutputName = outputSelection.endOutputName;

    m_impl->available = true;
    return true;
#else
    return false;
#endif
}

bool QaExtractiveModel::isAvailable() const
{
    return m_impl->available;
}

bool QaExtractiveModel::warmup() const
{
#if BS_WITH_ONNX
    if (!m_impl->available || !m_impl->session || !m_impl->tokenizer) {
        return false;
    }

    const WordPieceTokenizer::PairEncoding encoded =
        m_impl->tokenizer->tokenizePair(
            QStringLiteral("what is this"),
            QStringLiteral("this is simple context for answer"));
    if (encoded.inputIds.empty()) {
        return false;
    }

    const int64_t seqLen = static_cast<int64_t>(encoded.inputIds.size());
    const std::array<int64_t, 2> inputShape = {1, seqLen};

    try {
        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
            OrtAllocatorType::OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(encoded.inputIds.data()),
            encoded.inputIds.size(),
            inputShape.data(),
            static_cast<size_t>(inputShape.size()));

        std::vector<Ort::Value> inputTensors;
        std::vector<const char*> inputNamePtrs;
        inputTensors.reserve(m_impl->inputNames.size());
        inputNamePtrs.reserve(m_impl->inputNames.size());

        for (const std::string& inputName : m_impl->inputNames) {
            if (inputName == "input_ids") {
                inputTensors.push_back(std::move(inputIds));
            } else if (inputName == "attention_mask") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(encoded.attentionMask.data()),
                    encoded.attentionMask.size(),
                    inputShape.data(),
                    static_cast<size_t>(inputShape.size())));
            } else if (inputName == "token_type_ids") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(encoded.tokenTypeIds.data()),
                    encoded.tokenTypeIds.size(),
                    inputShape.data(),
                    static_cast<size_t>(inputShape.size())));
            } else {
                return false;
            }
            inputNamePtrs.push_back(inputName.c_str());
        }

        const char* outputNamePtrs[2] = {
            m_impl->startOutputName.c_str(),
            m_impl->endOutputName.c_str(),
        };

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            inputNamePtrs.data(),
            inputTensors.data(),
            inputTensors.size(),
            outputNamePtrs,
            2);

        if (outputs.size() < 2 || !outputs[0].IsTensor() || !outputs[1].IsTensor()) {
            return false;
        }

        const size_t requiredLogitCount = static_cast<size_t>(seqLen);
        const Ort::TensorTypeAndShapeInfo startInfo =
            outputs[0].GetTensorTypeAndShapeInfo();
        const Ort::TensorTypeAndShapeInfo endInfo =
            outputs[1].GetTensorTypeAndShapeInfo();
        if (startInfo.GetElementCount() < requiredLogitCount
            || endInfo.GetElementCount() < requiredLogitCount) {
            return false;
        }

        return outputs[0].GetTensorData<float>() != nullptr
            && outputs[1].GetTensorData<float>() != nullptr;
    } catch (const Ort::Exception&) {
        return false;
    }
#else
    return false;
#endif
}

QaExtractiveModel::Answer QaExtractiveModel::extract(const QString& query,
                                                     const QString& context,
                                                     int maxAnswerChars) const
{
    Answer out;
#if BS_WITH_ONNX
    if (!m_impl->available || !m_impl->session || !m_impl->tokenizer) {
        return out;
    }
    if (query.trimmed().isEmpty() || context.trimmed().isEmpty()) {
        return out;
    }

    const WordPieceTokenizer::PairEncoding encoded =
        m_impl->tokenizer->tokenizePair(query, context);
    if (encoded.inputIds.empty()) {
        return out;
    }

    const int64_t seqLen = static_cast<int64_t>(encoded.inputIds.size());
    const std::array<int64_t, 2> inputShape = {1, seqLen};

    try {
        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
            OrtAllocatorType::OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(encoded.inputIds.data()),
            encoded.inputIds.size(),
            inputShape.data(),
            static_cast<size_t>(inputShape.size()));
        std::vector<Ort::Value> inputTensors;
        std::vector<const char*> inputNamePtrs;
        inputTensors.reserve(m_impl->inputNames.size());
        inputNamePtrs.reserve(m_impl->inputNames.size());

        for (const std::string& inputName : m_impl->inputNames) {
            if (inputName == "input_ids") {
                inputTensors.push_back(std::move(inputIds));
            } else if (inputName == "attention_mask") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(encoded.attentionMask.data()),
                    encoded.attentionMask.size(),
                    inputShape.data(),
                    static_cast<size_t>(inputShape.size())));
            } else if (inputName == "token_type_ids") {
                inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(
                    memoryInfo,
                    const_cast<int64_t*>(encoded.tokenTypeIds.data()),
                    encoded.tokenTypeIds.size(),
                    inputShape.data(),
                    static_cast<size_t>(inputShape.size())));
            } else {
                return out;
            }
            inputNamePtrs.push_back(inputName.c_str());
        }
        const char* outputNamePtrs[2] = {
            m_impl->startOutputName.c_str(),
            m_impl->endOutputName.c_str(),
        };

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            inputNamePtrs.data(), inputTensors.data(), inputTensors.size(),
            outputNamePtrs, 2);

        if (outputs.size() < 2 || !outputs[0].IsTensor() || !outputs[1].IsTensor()) {
            return out;
        }

        const size_t requiredLogitCount = static_cast<size_t>(seqLen);
        const Ort::TensorTypeAndShapeInfo startInfo =
            outputs[0].GetTensorTypeAndShapeInfo();
        const Ort::TensorTypeAndShapeInfo endInfo =
            outputs[1].GetTensorTypeAndShapeInfo();
        if (startInfo.GetElementCount() < requiredLogitCount
            || endInfo.GetElementCount() < requiredLogitCount) {
            return out;
        }

        const float* startLogits = outputs[0].GetTensorData<float>();
        const float* endLogits = outputs[1].GetTensorData<float>();
        if (!startLogits || !endLogits) {
            return out;
        }

        int contextStart = -1;
        int contextEnd = -1;
        for (int i = 0; i < static_cast<int>(seqLen); ++i) {
            if (encoded.attentionMask[static_cast<size_t>(i)] == 1
                && encoded.tokenTypeIds[static_cast<size_t>(i)] == 1) {
                if (contextStart < 0) {
                    contextStart = i;
                }
                contextEnd = i;
            }
        }
        if (contextStart < 0 || contextEnd < contextStart) {
            return out;
        }

        // Exclude the trailing [SEP] token from context span search.
        if (contextEnd > contextStart
            && encoded.inputIds[static_cast<size_t>(contextEnd)] == 102) {
            --contextEnd;
        }
        if (contextEnd < contextStart) {
            return out;
        }

        constexpr int kMaxSpanTokens = 30;
        const qa_extractive_detail::SpanSelection bestSpan =
            qa_extractive_detail::selectBestSpan(startLogits,
                                                 endLogits,
                                                 contextStart,
                                                 contextEnd,
                                                 kMaxSpanTokens);
        if (!bestSpan.available) {
            return out;
        }

        const int contextTokenCount = std::max(1, contextEnd - contextStart + 1);
        const int centerToken = (bestSpan.startToken + bestSpan.endToken) / 2;
        const double relativeCenter = std::clamp(
            static_cast<double>(centerToken - contextStart)
                / static_cast<double>(std::max(1, contextTokenCount - 1)),
            0.0, 1.0);
        const int centerChar = static_cast<int>(
            relativeCenter * static_cast<double>(
                                 std::max(0, static_cast<int>(context.size()) - 1)));

        const QString answerText = extractSentenceAround(context, centerChar, maxAnswerChars);
        if (answerText.isEmpty()) {
            return out;
        }

        out.available = true;
        out.answer = answerText;
        out.rawScore = bestSpan.rawScore;
        out.confidence = bestSpan.confidence;
        out.startToken = bestSpan.startToken;
        out.endToken = bestSpan.endToken;
        return out;
    } catch (const Ort::Exception&) {
        return out;
    }
#else
    Q_UNUSED(query);
    Q_UNUSED(context);
    Q_UNUSED(maxAnswerChars);
    return out;
#endif
}

} // namespace bs
