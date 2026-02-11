#pragma once

#include <QString>

#include <memory>
#include <string>

namespace bs {

class ModelRegistry;

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
    Answer extract(const QString& query, const QString& context, int maxAnswerChars = 240) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_role;
};

} // namespace bs

