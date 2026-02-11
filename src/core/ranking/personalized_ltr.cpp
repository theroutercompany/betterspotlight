#include "core/ranking/personalized_ltr.h"

#include "vendor/sqlite/sqlite3.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace bs {

namespace {

constexpr const char* kCountInteractionsSql = "SELECT COUNT(*) FROM interactions";
constexpr const char* kTop3RateSql = R"(
    SELECT AVG(CASE WHEN result_position <= 3 THEN 1.0 ELSE 0.0 END)
    FROM interactions
)";

} // namespace

PersonalizedLtr::PersonalizedLtr(QString modelPath)
    : m_modelPath(std::move(modelPath))
{
}

double PersonalizedLtr::clamp(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
}

PersonalizedLtr::Weights PersonalizedLtr::defaultWeights()
{
    return Weights{};
}

bool PersonalizedLtr::initialize(sqlite3* db)
{
    m_available = loadModel();
    if (!m_available && db != nullptr) {
        m_available = maybeRetrain(db, 200);
    }
    return m_available;
}

bool PersonalizedLtr::isAvailable() const
{
    return m_available;
}

int PersonalizedLtr::countInteractions(sqlite3* db) const
{
    if (!db) {
        return 0;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kCountInteractionsSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

double PersonalizedLtr::computeTop3SelectionRate(sqlite3* db) const
{
    if (!db) {
        return 0.0;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kTop3RateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0.0;
    }

    double rate = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        rate = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return clamp(rate, 0.0, 1.0);
}

bool PersonalizedLtr::maybeRetrain(sqlite3* db, int minInteractions)
{
    const int interactions = countInteractions(db);
    if (interactions < std::max(minInteractions, 1)) {
        return false;
    }

    const double top3Rate = computeTop3SelectionRate(db);
    const double interactionScale = clamp(static_cast<double>(interactions) / 2000.0, 0.0, 1.0);

    Weights trained = defaultWeights();
    trained.semanticWeight = 1.6 + (1.4 * top3Rate);
    trained.crossEncoderWeight = 1.8 + (1.6 * top3Rate);
    trained.feedbackWeight = 0.8 + (1.2 * interactionScale);
    trained.routerWeight = 0.8 + (0.8 * top3Rate);
    trained.semanticNeedWeight = 1.2 + (0.8 * top3Rate);
    trained.exactMatchWeight = 0.9;
    trained.pathCodePenalty = -1.0;
    trained.bias = -2.2 + (0.4 * top3Rate);

    m_weights = trained;
    m_modelVersion = QStringLiteral("local_ltr_%1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddhhmmss")));
    m_available = saveModel();
    return m_available;
}

bool PersonalizedLtr::loadModel()
{
    QFile file(m_modelPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject w = root.value(QStringLiteral("weights")).toObject();

    m_weights.semanticWeight = w.value(QStringLiteral("semanticWeight")).toDouble(m_weights.semanticWeight);
    m_weights.crossEncoderWeight =
        w.value(QStringLiteral("crossEncoderWeight")).toDouble(m_weights.crossEncoderWeight);
    m_weights.feedbackWeight = w.value(QStringLiteral("feedbackWeight")).toDouble(m_weights.feedbackWeight);
    m_weights.routerWeight = w.value(QStringLiteral("routerWeight")).toDouble(m_weights.routerWeight);
    m_weights.semanticNeedWeight =
        w.value(QStringLiteral("semanticNeedWeight")).toDouble(m_weights.semanticNeedWeight);
    m_weights.exactMatchWeight = w.value(QStringLiteral("exactMatchWeight")).toDouble(m_weights.exactMatchWeight);
    m_weights.pathCodePenalty = w.value(QStringLiteral("pathCodePenalty")).toDouble(m_weights.pathCodePenalty);
    m_weights.bias = w.value(QStringLiteral("bias")).toDouble(m_weights.bias);

    m_modelVersion = root.value(QStringLiteral("version")).toString(QStringLiteral("local_ltr"));
    return true;
}

bool PersonalizedLtr::saveModel() const
{
    QJsonObject weights;
    weights[QStringLiteral("semanticWeight")] = m_weights.semanticWeight;
    weights[QStringLiteral("crossEncoderWeight")] = m_weights.crossEncoderWeight;
    weights[QStringLiteral("feedbackWeight")] = m_weights.feedbackWeight;
    weights[QStringLiteral("routerWeight")] = m_weights.routerWeight;
    weights[QStringLiteral("semanticNeedWeight")] = m_weights.semanticNeedWeight;
    weights[QStringLiteral("exactMatchWeight")] = m_weights.exactMatchWeight;
    weights[QStringLiteral("pathCodePenalty")] = m_weights.pathCodePenalty;
    weights[QStringLiteral("bias")] = m_weights.bias;

    QJsonObject root;
    root[QStringLiteral("version")] = m_modelVersion;
    root[QStringLiteral("trainedAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("weights")] = weights;

    QFile file(m_modelPath);
    const QString dirPath = QFileInfo(file).absolutePath();
    QDir().mkpath(dirPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

double PersonalizedLtr::apply(std::vector<SearchResult>& results,
                              const LtrContext& context,
                              int maxCandidates) const
{
    if (!m_available || results.empty() || maxCandidates <= 0) {
        return 0.0;
    }

    const int limit = std::min(maxCandidates, static_cast<int>(results.size()));
    double deltaTop10 = 0.0;

    for (int i = 0; i < limit; ++i) {
        SearchResult& result = results[static_cast<size_t>(i)];
        const double semanticFeature = clamp(result.semanticNormalized, 0.0, 1.0);
        const double crossFeature = clamp(result.crossEncoderScore, 0.0, 1.0);
        const double feedbackFeature = clamp(
            (result.scoreBreakdown.feedbackBoost + result.scoreBreakdown.frequencyBoost) / 40.0,
            0.0, 1.0);
        const double routerFeature = clamp(context.routerConfidence, 0.0, 1.0);
        const double semanticNeedFeature = clamp(context.semanticNeedScore, 0.0, 1.0);
        const double exactFeature = (result.matchType == MatchType::ExactName
                                     || result.matchType == MatchType::PrefixName) ? 1.0 : 0.0;

        double delta = m_weights.bias
            + (m_weights.semanticWeight * semanticFeature)
            + (m_weights.crossEncoderWeight * crossFeature)
            + (m_weights.feedbackWeight * feedbackFeature)
            + (m_weights.routerWeight * routerFeature)
            + (m_weights.semanticNeedWeight * semanticNeedFeature)
            + (m_weights.exactMatchWeight * exactFeature);

        if (context.queryClass == QueryClass::PathOrCode && semanticFeature > 0.7) {
            delta += m_weights.pathCodePenalty;
        }
        delta = clamp(delta, -8.0, 8.0);
        result.score += delta;
        result.scoreBreakdown.m2SignalBoost += delta;
        if (i < 10) {
            deltaTop10 += delta;
        }
    }

    std::stable_sort(results.begin(), results.end(),
                     [](const SearchResult& lhs, const SearchResult& rhs) {
                         if (lhs.score != rhs.score) {
                             return lhs.score > rhs.score;
                         }
                         return lhs.itemId < rhs.itemId;
                     });
    return deltaTop10;
}

QString PersonalizedLtr::modelVersion() const
{
    return m_modelVersion;
}

} // namespace bs
