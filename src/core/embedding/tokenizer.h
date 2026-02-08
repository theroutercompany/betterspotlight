#pragma once

#include <QString>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs {

struct TokenizerOutput {
    std::vector<int64_t> inputIds;
    std::vector<int64_t> attentionMask;
    std::vector<int64_t> tokenTypeIds;
    int seqLength = 0;
};

struct BatchTokenizerOutput {
    std::vector<int64_t> inputIds;
    std::vector<int64_t> attentionMask;
    std::vector<int64_t> tokenTypeIds;
    int batchSize = 0;
    int seqLength = 0;
};

class WordPieceTokenizer {
public:
    explicit WordPieceTokenizer(const QString& vocabPath);

    bool isLoaded() const;

    TokenizerOutput tokenize(const QString& text, int padToLength = 0) const;
    BatchTokenizerOutput tokenizeBatch(const std::vector<QString>& texts) const;

private:
    static constexpr int kPadTokenId = 0;
    static constexpr int kUnkTokenId = 100;
    static constexpr int kClsTokenId = 101;
    static constexpr int kSepTokenId = 102;
    static constexpr int kMaxSequenceLength = 512;
    static constexpr int kMaxContentTokens = kMaxSequenceLength - 2;

    QString normalize(const QString& text) const;
    std::vector<int64_t> tokenizeContent(const QString& normalizedText) const;
    void appendWordPieces(const QString& token, std::vector<int64_t>* output) const;

    std::unordered_map<std::string, int> m_vocab;
    bool m_loaded = false;
};

} // namespace bs
