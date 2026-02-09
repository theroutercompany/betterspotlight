#include "core/query/rules_engine.h"

#include "core/query/doctype_classifier.h"
#include "core/query/entity_extractor.h"
#include "core/query/query_normalizer.h"
#include "core/query/stopwords.h"
#include "core/query/temporal_parser.h"

namespace bs {

StructuredQuery RulesEngine::analyze(const QString& originalQuery)
{
    StructuredQuery sq;
    sq.originalQuery = originalQuery;

    // Normalize the query (lowercased, noise-stripped)
    NormalizedQuery normalized = QueryNormalizer::normalize(originalQuery);
    sq.cleanedQuery = normalized.normalized;

    // Extract entities from the original (case-preserved) query
    sq.entities = EntityExtractor::extract(originalQuery);

    // Parse temporal signals from the original query
    sq.temporal = TemporalParser::parse(originalQuery);

    // Classify document-type intent from the cleaned (lowercased) query
    sq.docTypeIntent = DoctypeClassifier::classify(sq.cleanedQuery);

    // Location hints
    if (sq.cleanedQuery.contains(QStringLiteral("downloads"))) {
        sq.locationHints.push_back(QStringLiteral("downloads"));
    }
    if (sq.cleanedQuery.contains(QStringLiteral("documents"))) {
        sq.locationHints.push_back(QStringLiteral("documents"));
    }
    if (sq.cleanedQuery.contains(QStringLiteral("desktop"))) {
        sq.locationHints.push_back(QStringLiteral("desktop"));
    }

    // Key tokens: tokenize, remove stopwords, keep tokens >= 3 chars
    const QStringList tokens =
        sq.cleanedQuery.split(QChar(' '), Qt::SkipEmptyParts);
    const QSet<QString>& stopwords = queryStopwords();
    for (const QString& token : tokens) {
        if (token.size() >= 3 && !stopwords.contains(token)) {
            sq.keyTokens.push_back(token);
        }
    }

    // NLU confidence: always 0.0 for rules-based engine
    sq.nluConfidence = 0.0f;

    return sq;
}

} // namespace bs
