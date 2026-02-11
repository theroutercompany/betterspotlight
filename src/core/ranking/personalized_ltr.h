#pragma once

#include "core/query/structured_query.h"
#include "core/shared/search_result.h"

#include <QString>

#include <vector>

struct sqlite3;

namespace bs {

struct LtrContext {
    QueryClass queryClass = QueryClass::Unknown;
    float routerConfidence = 0.0f;
    float semanticNeedScore = 0.0f;
};

class PersonalizedLtr {
public:
    explicit PersonalizedLtr(QString modelPath);

    bool initialize(sqlite3* db);
    bool isAvailable() const;
    bool maybeRetrain(sqlite3* db, int minInteractions = 200);
    double apply(std::vector<SearchResult>& results,
                 const LtrContext& context,
                 int maxCandidates = 100) const;
    QString modelVersion() const;

private:
    struct Weights {
        double semanticWeight = 2.0;
        double crossEncoderWeight = 2.0;
        double feedbackWeight = 1.0;
        double routerWeight = 1.0;
        double semanticNeedWeight = 1.5;
        double exactMatchWeight = 0.8;
        double pathCodePenalty = -1.2;
        double bias = -2.6;
    };

    static double clamp(double value, double low, double high);
    static Weights defaultWeights();
    bool loadModel();
    bool saveModel() const;
    int countInteractions(sqlite3* db) const;
    double computeTop3SelectionRate(sqlite3* db) const;

    QString m_modelPath;
    QString m_modelVersion = QStringLiteral("cold_start");
    Weights m_weights = defaultWeights();
    bool m_available = false;
};

} // namespace bs

