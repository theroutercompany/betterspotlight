#pragma once

#include <QString>

#include <memory>
#include <string>
#include <vector>

namespace bs {

class ModelRegistry;

namespace qa_extractive_detail {

struct SpanSelection {
    bool available = false;
    int startToken = -1;
    int endToken = -1;
    double rawScore = 0.0;
    double confidence = 0.0;
};

struct OutputNameSelection {
    bool available = false;
    std::string startOutputName;
    std::string endOutputName;
};

double confidenceForRawScore(double rawScore);
SpanSelection selectBestSpan(const float* startLogits,
                             const float* endLogits,
                             int contextStart,
                             int contextEnd,
                             int maxSpanTokens);
OutputNameSelection selectOutputNames(const std::vector<std::string>& outputNames,
                                      bool allowSingleOutputFallback);

} // namespace qa_extractive_detail

class QaExtractiveModel {
public:
    struct Answer {
        bool available = false;
        QString answer;
        double confidence = 0.0;
        double rawScore = 0.0;
        int startToken = -1;
        int endToken = -1;
    };

    explicit QaExtractiveModel(ModelRegistry* registry, std::string role = "qa-extractive");
    ~QaExtractiveModel();

    bool initialize();
    bool isAvailable() const;
    bool warmup() const;
    Answer extract(const QString& query, const QString& context, int maxAnswerChars = 240) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_role;
};

} // namespace bs
