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

namespace {

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
    if (m_impl->inputNames.size() < 3) {
        m_impl->inputNames = {"input_ids", "attention_mask", "token_type_ids"};
    }

    const auto& outputNames = modelSession->outputNames();
    if (outputNames.size() >= 2) {
        m_impl->startOutputName = outputNames[0];
        m_impl->endOutputName = outputNames[1];
    } else {
        const auto itStart = std::find_if(outputNames.begin(), outputNames.end(),
                                          [](const std::string& value) {
                                              return value.find("start") != std::string::npos;
                                          });
        const auto itEnd = std::find_if(outputNames.begin(), outputNames.end(),
                                        [](const std::string& value) {
                                            return value.find("end") != std::string::npos;
                                        });
        if (itStart == outputNames.end() || itEnd == outputNames.end()) {
            return false;
        }
        m_impl->startOutputName = *itStart;
        m_impl->endOutputName = *itEnd;
    }

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
        Ort::Value attentionMask = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(encoded.attentionMask.data()),
            encoded.attentionMask.size(),
            inputShape.data(),
            static_cast<size_t>(inputShape.size()));
        Ort::Value tokenTypeIds = Ort::Value::CreateTensor<int64_t>(
            memoryInfo,
            const_cast<int64_t*>(encoded.tokenTypeIds.data()),
            encoded.tokenTypeIds.size(),
            inputShape.data(),
            static_cast<size_t>(inputShape.size()));

        std::array<Ort::Value, 3> inputTensors = {
            std::move(inputIds),
            std::move(attentionMask),
            std::move(tokenTypeIds),
        };

        const char* inputNamePtrs[3] = {
            m_impl->inputNames[0].c_str(),
            m_impl->inputNames[1].c_str(),
            m_impl->inputNames[2].c_str(),
        };
        const char* outputNamePtrs[2] = {
            m_impl->startOutputName.c_str(),
            m_impl->endOutputName.c_str(),
        };

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            inputNamePtrs, inputTensors.data(), 3,
            outputNamePtrs, 2);

        if (outputs.size() < 2 || !outputs[0].IsTensor() || !outputs[1].IsTensor()) {
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
        double bestScore = -std::numeric_limits<double>::infinity();
        int bestStart = -1;
        int bestEnd = -1;

        for (int s = contextStart; s <= contextEnd; ++s) {
            const int maxEnd = std::min(contextEnd, s + kMaxSpanTokens);
            for (int e = s; e <= maxEnd; ++e) {
                const double score = static_cast<double>(startLogits[s])
                    + static_cast<double>(endLogits[e]);
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

        const int contextTokenCount = std::max(1, contextEnd - contextStart + 1);
        const int centerToken = (bestStart + bestEnd) / 2;
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
        out.rawScore = bestScore;
        out.confidence = std::clamp(1.0 / (1.0 + std::exp(-(bestScore / 6.0))), 0.0, 1.0);
        out.startToken = bestStart;
        out.endToken = bestEnd;
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
