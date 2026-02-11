#include "core/query/query_router.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace bs {

namespace {

bool looksLikePathOrCode(const QString& queryLower)
{
    if (queryLower.contains(QLatin1Char('/'))
        || queryLower.contains(QLatin1Char('\\'))
        || queryLower.startsWith(QLatin1Char('.'))
        || queryLower.startsWith(QLatin1Char('~'))
        || queryLower.contains(QStringLiteral("::"))) {
        return true;
    }

    static const QRegularExpression extensionLikeToken(
        QStringLiteral(R"(\b[a-z0-9_\-]+\.[a-z0-9]{1,8}\b)"));
    if (extensionLikeToken.match(queryLower).hasMatch()) {
        return true;
    }

    static const QRegularExpression codePunctuation(
        QStringLiteral(R"([<>{}\[\]();=#])"));
    return codePunctuation.match(queryLower).hasMatch();
}

bool containsAny(const QString& lower, const std::initializer_list<const char*>& needles)
{
    for (const char* needle : needles) {
        if (lower.contains(QString::fromLatin1(needle))) {
            return true;
        }
    }
    return false;
}

float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

QString queryClassToString(QueryClass value)
{
    switch (value) {
    case QueryClass::NaturalLanguage:
        return QStringLiteral("natural_language");
    case QueryClass::PathOrCode:
        return QStringLiteral("path_or_code");
    case QueryClass::ShortAmbiguous:
        return QStringLiteral("short_ambiguous");
    case QueryClass::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

QString queryDomainToString(QueryDomain value)
{
    switch (value) {
    case QueryDomain::PersonalDocs:
        return QStringLiteral("personal_docs");
    case QueryDomain::DevCode:
        return QStringLiteral("dev_code");
    case QueryDomain::Finance:
        return QStringLiteral("finance");
    case QueryDomain::Media:
        return QStringLiteral("media");
    case QueryDomain::General:
        return QStringLiteral("general");
    case QueryDomain::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

QueryRouterResult QueryRouter::route(const QString& originalQuery,
                                     const QString& cleanedQuery,
                                     const std::vector<QString>& keyTokens)
{
    Q_UNUSED(originalQuery);

    QueryRouterResult result;
    if (cleanedQuery.trimmed().isEmpty()) {
        return result;
    }

    const QString lower = cleanedQuery.toLower();
    const bool pathOrCode = looksLikePathOrCode(lower);
    const int tokenCount = static_cast<int>(keyTokens.size());

    if (pathOrCode) {
        result.queryClass = QueryClass::PathOrCode;
        result.routerConfidence = 0.88f;
    } else if (tokenCount >= 3) {
        result.queryClass = QueryClass::NaturalLanguage;
        result.routerConfidence = 0.75f + std::min(0.15f, static_cast<float>(tokenCount - 3) * 0.03f);
    } else {
        result.queryClass = QueryClass::ShortAmbiguous;
        result.routerConfidence = tokenCount == 0 ? 0.45f : 0.60f;
    }

    if (containsAny(lower, {"cpp", "h", "js", "ts", "swift", "python", "go", "rust",
                            "function", "class", "method", "api", "endpoint",
                            "stacktrace", "exception", "build", "deploy"})) {
        result.queryDomain = QueryDomain::DevCode;
        result.queryDomainConfidence = 0.82f;
    } else if (containsAny(lower, {"invoice", "receipt", "budget", "tax", "bank",
                                   "statement", "expense", "payment"})) {
        result.queryDomain = QueryDomain::Finance;
        result.queryDomainConfidence = 0.80f;
    } else if (containsAny(lower, {"photo", "image", "screenshot", "video", "music",
                                   "podcast", "mp3", "mp4", "png", "jpg"})) {
        result.queryDomain = QueryDomain::Media;
        result.queryDomainConfidence = 0.78f;
    } else if (containsAny(lower, {"resume", "notes", "report", "proposal", "document",
                                   "contract", "letter", "spreadsheet"})) {
        result.queryDomain = QueryDomain::PersonalDocs;
        result.queryDomainConfidence = 0.74f;
    } else {
        result.queryDomain = QueryDomain::General;
        result.queryDomainConfidence = 0.52f;
    }

    float semanticNeed = 0.0f;
    switch (result.queryClass) {
    case QueryClass::NaturalLanguage:
        semanticNeed = 0.68f;
        break;
    case QueryClass::ShortAmbiguous:
        semanticNeed = 0.38f;
        break;
    case QueryClass::PathOrCode:
        semanticNeed = 0.20f;
        break;
    case QueryClass::Unknown:
    default:
        semanticNeed = 0.30f;
        break;
    }

    if (containsAny(lower, {"how", "what", "where", "plan", "overview", "design",
                            "architecture", "guide", "explain", "related"})) {
        semanticNeed += 0.12f;
    }
    if (containsAny(lower, {"pdf", "docx", "xlsx", "png", "jpg", "mp3", "zip"})) {
        semanticNeed -= 0.08f;
    }
    result.semanticNeedScore = clamp01(semanticNeed);
    result.routerConfidence = clamp01(result.routerConfidence);
    result.queryDomainConfidence = clamp01(result.queryDomainConfidence);
    result.valid = true;
    return result;
}

} // namespace bs

