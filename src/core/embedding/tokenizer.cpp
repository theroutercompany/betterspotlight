#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>
#include <QDebug>

#include <algorithm>
#include <utility>

#include "core/embedding/tokenizer.h"

namespace bs {

WordPieceTokenizer::WordPieceTokenizer(const QString& vocabPath)
{
    QFile file(vocabPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "WordPieceTokenizer failed to open vocab:" << vocabPath;
        return;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    int index = 0;
    while (!in.atEnd()) {
        const QString token = in.readLine().trimmed();
        if (!token.isEmpty()) {
            m_vocab.emplace(token.toStdString(), index);
        }
        ++index;
    }

    if (m_vocab.empty()) {
        qWarning() << "WordPieceTokenizer loaded empty vocab from" << vocabPath;
        return;
    }

    m_loaded = true;
}

bool WordPieceTokenizer::isLoaded() const
{
    return m_loaded;
}

QString WordPieceTokenizer::normalize(const QString& text) const
{
    QString normalized = text.toLower().normalized(QString::NormalizationForm_D);

    QString stripped;
    stripped.reserve(normalized.size());
    for (const QChar ch : normalized) {
        const QChar::Category category = ch.category();
        const bool isCombiningMark = category == QChar::Mark_NonSpacing
                                   || category == QChar::Mark_SpacingCombining
                                   || category == QChar::Mark_Enclosing;
        if (!isCombiningMark) {
            stripped.append(ch);
        }
    }

    static const QRegularExpression whitespaceRegex(QStringLiteral("\\s+"));
    stripped.replace(whitespaceRegex, QStringLiteral(" "));
    return stripped.trimmed();
}

void WordPieceTokenizer::appendWordPieces(const QString& token, std::vector<int64_t>* output) const
{
    if (!output || token.isEmpty() || static_cast<int>(output->size()) >= kMaxContentTokens) {
        return;
    }

    const int tokenLength = token.size();
    int start = 0;
    bool emittedUnknown = false;

    while (start < tokenLength) {
        int end = tokenLength;
        int matchedId = -1;
        int matchedEnd = start;

        while (end > start) {
            QString piece = token.mid(start, end - start);
            if (start > 0) {
                piece.prepend(QStringLiteral("##"));
            }

            const auto it = m_vocab.find(piece.toStdString());
            if (it != m_vocab.end()) {
                matchedId = it->second;
                matchedEnd = end;
                break;
            }

            --end;
        }

        if (matchedId < 0) {
            output->push_back(kUnkTokenId);
            emittedUnknown = true;
            break;
        }

        output->push_back(static_cast<int64_t>(matchedId));
        if (static_cast<int>(output->size()) >= kMaxContentTokens) {
            break;
        }
        start = matchedEnd;
    }

    if (!emittedUnknown && start < tokenLength && static_cast<int>(output->size()) < kMaxContentTokens) {
        output->push_back(kUnkTokenId);
    }
}

std::vector<int64_t> WordPieceTokenizer::tokenizeContent(const QString& normalizedText) const
{
    std::vector<int64_t> content;
    if (!m_loaded || normalizedText.isEmpty()) {
        return content;
    }

    const QStringList words = normalizedText.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        if (static_cast<int>(content.size()) >= kMaxContentTokens) {
            break;
        }
        appendWordPieces(word, &content);
    }

    if (static_cast<int>(content.size()) > kMaxContentTokens) {
        content.resize(kMaxContentTokens);
    }
    return content;
}

TokenizerOutput WordPieceTokenizer::tokenize(const QString& text, int padToLength) const
{
    TokenizerOutput output;
    if (!m_loaded) {
        return output;
    }

    std::vector<int64_t> content = tokenizeContent(normalize(text));

    output.inputIds.reserve(static_cast<size_t>(content.size()) + 2);
    output.inputIds.push_back(kClsTokenId);
    output.inputIds.insert(output.inputIds.end(), content.begin(), content.end());
    output.inputIds.push_back(kSepTokenId);

    const int unpaddedLength = static_cast<int>(output.inputIds.size());
    const int clampedPadLength = std::min(padToLength, kMaxSequenceLength);
    const int targetLength = std::max(unpaddedLength, clampedPadLength);

    output.attentionMask.assign(static_cast<size_t>(targetLength), 0);
    output.tokenTypeIds.assign(static_cast<size_t>(targetLength), 0);

    for (int i = 0; i < unpaddedLength; ++i) {
        output.attentionMask[static_cast<size_t>(i)] = 1;
    }

    if (targetLength > unpaddedLength) {
        output.inputIds.resize(static_cast<size_t>(targetLength), kPadTokenId);
    }

    output.seqLength = targetLength;
    return output;
}

WordPieceTokenizer::PairEncoding WordPieceTokenizer::tokenizePair(
    const QString& textA, const QString& textB, int padToLength) const
{
    PairEncoding output;
    if (!m_loaded) {
        return output;
    }

    std::vector<int64_t> tokensA = tokenizeContent(normalize(textA));
    std::vector<int64_t> tokensB = tokenizeContent(normalize(textB));

    // Truncate to fit within kMaxPairContentTokens (room for [CLS] + 2x[SEP])
    const int budget = kMaxPairContentTokens;
    const int totalTokens = static_cast<int>(tokensA.size() + tokensB.size());
    if (totalTokens > budget) {
        // Truncate B first (to half budget), then A
        const int halfBudget = budget / 2;
        if (static_cast<int>(tokensB.size()) > halfBudget) {
            tokensB.resize(static_cast<size_t>(halfBudget));
        }
        const int remainingBudget = budget - static_cast<int>(tokensB.size());
        if (static_cast<int>(tokensA.size()) > remainingBudget) {
            tokensA.resize(static_cast<size_t>(remainingBudget));
        }
    }

    // Build: [CLS] tokensA [SEP] tokensB [SEP]
    const int seqLen = 3 + static_cast<int>(tokensA.size() + tokensB.size());
    output.inputIds.reserve(static_cast<size_t>(seqLen));
    output.inputIds.push_back(kClsTokenId);
    output.inputIds.insert(output.inputIds.end(), tokensA.begin(), tokensA.end());
    output.inputIds.push_back(kSepTokenId);
    output.inputIds.insert(output.inputIds.end(), tokensB.begin(), tokensB.end());
    output.inputIds.push_back(kSepTokenId);

    // tokenTypeIds: 0 for [CLS] + A + first [SEP], 1 for B + second [SEP]
    const int segALen = 1 + static_cast<int>(tokensA.size()) + 1; // [CLS] + A + [SEP]
    const int segBLen = static_cast<int>(tokensB.size()) + 1;      // B + [SEP]
    output.tokenTypeIds.assign(static_cast<size_t>(segALen), 0);
    output.tokenTypeIds.resize(static_cast<size_t>(segALen + segBLen), 1);

    // attentionMask: 1 for real tokens
    output.attentionMask.assign(static_cast<size_t>(seqLen), 1);

    // Pad if requested
    const int clampedPadLength = std::min(padToLength, kMaxSequenceLength);
    const int targetLength = std::max(seqLen, clampedPadLength);
    if (targetLength > seqLen) {
        output.inputIds.resize(static_cast<size_t>(targetLength), kPadTokenId);
        output.attentionMask.resize(static_cast<size_t>(targetLength), 0);
        output.tokenTypeIds.resize(static_cast<size_t>(targetLength), 0);
    }

    return output;
}

WordPieceTokenizer::PairBatchEncoding WordPieceTokenizer::tokenizePairBatch(
    const std::vector<std::pair<QString, QString>>& pairs) const
{
    PairBatchEncoding batch;
    if (!m_loaded || pairs.empty()) {
        return batch;
    }

    // First pass: encode all pairs and find max length
    std::vector<PairEncoding> encoded;
    encoded.reserve(pairs.size());
    int maxLength = 0;
    for (const auto& [textA, textB] : pairs) {
        PairEncoding pe = tokenizePair(textA, textB);
        maxLength = std::max(maxLength, static_cast<int>(pe.inputIds.size()));
        encoded.push_back(std::move(pe));
    }

    // Second pass: pad all to max length and flatten
    batch.batchSize = static_cast<int>(pairs.size());
    batch.sequenceLength = maxLength;
    const size_t totalElements = static_cast<size_t>(batch.batchSize) * static_cast<size_t>(maxLength);
    batch.inputIds.reserve(totalElements);
    batch.attentionMask.reserve(totalElements);
    batch.tokenTypeIds.reserve(totalElements);

    for (auto& pe : encoded) {
        const int curLen = static_cast<int>(pe.inputIds.size());
        if (curLen < maxLength) {
            pe.inputIds.resize(static_cast<size_t>(maxLength), kPadTokenId);
            pe.attentionMask.resize(static_cast<size_t>(maxLength), 0);
            pe.tokenTypeIds.resize(static_cast<size_t>(maxLength), 0);
        }
        batch.inputIds.insert(batch.inputIds.end(), pe.inputIds.begin(), pe.inputIds.end());
        batch.attentionMask.insert(batch.attentionMask.end(), pe.attentionMask.begin(), pe.attentionMask.end());
        batch.tokenTypeIds.insert(batch.tokenTypeIds.end(), pe.tokenTypeIds.begin(), pe.tokenTypeIds.end());
    }

    return batch;
}

BatchTokenizerOutput WordPieceTokenizer::tokenizeBatch(const std::vector<QString>& texts) const
{
    BatchTokenizerOutput batch;
    if (!m_loaded || texts.empty()) {
        return batch;
    }

    std::vector<TokenizerOutput> tokenized;
    tokenized.reserve(texts.size());

    int maxLength = 0;
    for (const QString& text : texts) {
        TokenizerOutput single = tokenize(text);
        maxLength = std::max(maxLength, single.seqLength);
        tokenized.push_back(std::move(single));
    }

    batch.batchSize = static_cast<int>(texts.size());
    batch.seqLength = maxLength;
    batch.inputIds.reserve(static_cast<size_t>(batch.batchSize * batch.seqLength));
    batch.attentionMask.reserve(static_cast<size_t>(batch.batchSize * batch.seqLength));
    batch.tokenTypeIds.reserve(static_cast<size_t>(batch.batchSize * batch.seqLength));

    for (TokenizerOutput& row : tokenized) {
        if (row.seqLength < maxLength) {
            row.inputIds.resize(static_cast<size_t>(maxLength), kPadTokenId);
            row.attentionMask.resize(static_cast<size_t>(maxLength), 0);
            row.tokenTypeIds.resize(static_cast<size_t>(maxLength), 0);
            row.seqLength = maxLength;
        }

        batch.inputIds.insert(batch.inputIds.end(), row.inputIds.begin(), row.inputIds.end());
        batch.attentionMask.insert(batch.attentionMask.end(), row.attentionMask.begin(), row.attentionMask.end());
        batch.tokenTypeIds.insert(batch.tokenTypeIds.end(), row.tokenTypeIds.begin(), row.tokenTypeIds.end());
    }

    return batch;
}

} // namespace bs
