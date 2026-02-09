#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <optional>
#include <cstdint>

#include <sqlite3.h>

namespace bs {

// TypoLexicon - builds a searchable vocabulary from FTS5 for typo correction.
//
// Lazily initialized on first use. Caches terms by first-letter bucket
// for O(bucket_size) correction instead of O(total_terms).
class TypoLexicon {
public:
    TypoLexicon() = default;

    // Build lexicon from the fts5vocab virtual table.
    // Must be called with the same db that has search_index.
    // Returns false if fts5vocab is not available.
    bool build(sqlite3* db);

    // Whether the lexicon has been built.
    bool isReady() const { return m_ready; }

    // Total unique terms in the lexicon.
    int termCount() const { return m_totalTerms; }

    struct Correction {
        QString corrected;     // The corrected term from the lexicon
        int editDistance = 0;  // How many edits away from input
        int64_t docCount = 0;  // How many documents contain this term
    };

    // Find the best correction for a misspelled token.
    // Uses Damerau-Levenshtein first, then a double-letter compression fallback.
    // Returns std::nullopt if no correction found within maxDistance.
    // maxDistance: 1 for tokens <8 chars, 2 for longer tokens.
    std::optional<Correction> correct(const QString& token, int maxDistance = 1) const;

    // Check if a token exists exactly in the lexicon.
    bool contains(const QString& token) const;

    // Clear the lexicon.
    void clear();

private:
    struct Term {
        QString text;
        int64_t docCount = 0;  // from fts5vocab 'row' mode: number of rows containing term
    };

    // Damerau-Levenshtein edit distance, capped at maxDistance+1 for early exit.
    static int editDistance(const QString& a, const QString& b, int maxDist);

    // First-letter buckets for fast lookup
    QHash<QChar, QVector<Term>> m_buckets;
    int m_totalTerms = 0;
    bool m_ready = false;

    // Caps to protect memory and latency
    static constexpr int kMaxTermsPerBucket = 5000;
    static constexpr int kMaxTotalTerms = 100000;
    static constexpr int kMaxFileNameTerms = 50000;
    static constexpr int kMinTermLength = 2;
};

} // namespace bs
