#include "core/index/typo_lexicon.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace bs {

namespace {

QVector<QChar> adjacentFirstChars(QChar c)
{
    const QChar lower = c.toLower();
    QString neighbors;

    switch (lower.unicode()) {
    case 'a': neighbors = QStringLiteral("qwsz"); break;
    case 'b': neighbors = QStringLiteral("vghn"); break;
    case 'c': neighbors = QStringLiteral("xdfv"); break;
    case 'd': neighbors = QStringLiteral("erfcxs"); break;
    case 'e': neighbors = QStringLiteral("rdsw"); break;
    case 'f': neighbors = QStringLiteral("rtgvcd"); break;
    case 'g': neighbors = QStringLiteral("tyhbvf"); break;
    case 'h': neighbors = QStringLiteral("yujnbg"); break;
    case 'i': neighbors = QStringLiteral("okju"); break;
    case 'j': neighbors = QStringLiteral("uikmnh"); break;
    case 'k': neighbors = QStringLiteral("iolmnj"); break;
    case 'l': neighbors = QStringLiteral("opk"); break;
    case 'm': neighbors = QStringLiteral("njk"); break;
    case 'n': neighbors = QStringLiteral("bhjm"); break;
    case 'o': neighbors = QStringLiteral("plki"); break;
    case 'p': neighbors = QStringLiteral("lo"); break;
    case 'q': neighbors = QStringLiteral("wa"); break;
    case 'r': neighbors = QStringLiteral("tfde"); break;
    case 's': neighbors = QStringLiteral("wedxza"); break;
    case 't': neighbors = QStringLiteral("ygfr"); break;
    case 'u': neighbors = QStringLiteral("ijhy"); break;
    case 'v': neighbors = QStringLiteral("cfgb"); break;
    case 'w': neighbors = QStringLiteral("qeas"); break;
    case 'x': neighbors = QStringLiteral("zsdc"); break;
    case 'y': neighbors = QStringLiteral("uhgt"); break;
    case 'z': neighbors = QStringLiteral("asx"); break;
    default:
        break;
    }

    QVector<QChar> result;
    result.reserve(neighbors.size() + 1);
    result.push_back(lower);
    for (const QChar ch : neighbors) {
        result.push_back(ch);
    }
    return result;
}

QString compressRuns(const QString& s)
{
    if (s.isEmpty()) {
        return s;
    }

    QString compressed;
    compressed.reserve(s.size());
    compressed.append(s[0]);

    for (int i = 1; i < s.size(); ++i) {
        if (s[i] == s[i - 1]) {
            continue;
        }
        compressed.append(s[i]);
    }

    return compressed;
}

} // namespace

bool TypoLexicon::build(sqlite3* db)
{
    clear();
    if (!db) {
        return false;
    }

    char* errMsg = nullptr;
    const char* createSql =
        "CREATE VIRTUAL TABLE IF NOT EXISTS search_index_vocab "
        "USING fts5vocab(search_index, 'row');";
    int rc = sqlite3_exec(db, createSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT term, doc FROM search_index_vocab ORDER BY doc DESC",
        -1,
        &stmt,
        nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* termText = sqlite3_column_text(stmt, 0);
        if (!termText) {
            continue;
        }

        const QString term = QString::fromUtf8(reinterpret_cast<const char*>(termText)).toLower();
        if (term.size() < kMinTermLength) {
            continue;
        }

        const QChar bucketKey = term.front();
        QVector<Term>& bucket = m_buckets[bucketKey];

        if (bucket.size() >= kMaxTermsPerBucket) {
            continue;
        }
        if (m_totalTerms >= kMaxTotalTerms) {
            break;
        }

        bucket.push_back(Term{term, sqlite3_column_int64(stmt, 1)});
        ++m_totalTerms;
    }

    sqlite3_finalize(stmt);

    // Augment lexicon with unstemmed filename tokens. FTS5 vocab is porter-stemmed
    // ("break" not "breaking"), so typos like "Braeking" (dist=3 from "break") are
    // uncorrectable. Raw filename words provide unstemmed correction targets.
    sqlite3_stmt* nameStmt = nullptr;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT DISTINCT name FROM items WHERE name IS NOT NULL AND name != ''",
        -1,
        &nameStmt,
        nullptr);
    if (rc == SQLITE_OK) {
        QSet<QString> existingTerms;
        for (auto it = m_buckets.constBegin(); it != m_buckets.constEnd(); ++it) {
            for (const Term& t : it.value()) {
                existingTerms.insert(t.text);
            }
        }

        static const QRegularExpression wordSplitter(QStringLiteral(R"([^A-Za-z0-9]+)"));
        int fileNameTerms = 0;
        while (sqlite3_step(nameStmt) == SQLITE_ROW && fileNameTerms < kMaxFileNameTerms) {
            const unsigned char* nameText = sqlite3_column_text(nameStmt, 0);
            if (!nameText) {
                continue;
            }
            const QString name = QString::fromUtf8(reinterpret_cast<const char*>(nameText));
            const QStringList words = name.split(wordSplitter, Qt::SkipEmptyParts);
            for (const QString& word : words) {
                const QString lower = word.toLower();
                if (lower.size() < kMinTermLength) {
                    continue;
                }
                if (existingTerms.contains(lower)) {
                    continue;
                }

                const QChar bucketKey = lower.front();
                QVector<Term>& bucket = m_buckets[bucketKey];
                if (fileNameTerms >= kMaxFileNameTerms) {
                    break;
                }

                bucket.push_back(Term{lower, 1});
                existingTerms.insert(lower);
                ++m_totalTerms;
                ++fileNameTerms;
            }
        }
        sqlite3_finalize(nameStmt);
    }

    m_ready = true;
    return true;
}

std::optional<TypoLexicon::Correction> TypoLexicon::correct(const QString& token, int maxDistance) const
{
    if (!m_ready || token.size() < kMinTermLength || maxDistance < 0) {
        return std::nullopt;
    }

    const QString normalized = token.toLower();
    if (normalized.isEmpty()) {
        return std::nullopt;
    }

    std::optional<Correction> best;
    const QVector<QChar> keys = adjacentFirstChars(normalized.front());

    for (const QChar key : keys) {
        auto it = m_buckets.constFind(key);
        if (it == m_buckets.constEnd()) {
            continue;
        }

        const QVector<Term>& bucket = it.value();
        for (const Term& candidate : bucket) {
            if (std::abs(candidate.text.size() - normalized.size()) > maxDistance) {
                continue;
            }

            const int dist = editDistance(normalized, candidate.text, maxDistance);
            if (dist > maxDistance) {
                continue;
            }

            if (!best.has_value()
                || dist < best->editDistance
                || (dist == best->editDistance && candidate.docCount > best->docCount)) {
                best = Correction{candidate.text, dist, candidate.docCount};
            }
        }
    }

    if (best.has_value()) {
        return best;
    }

    const QString compressedInput = compressRuns(normalized);
    for (const QChar key : keys) {
        auto it = m_buckets.constFind(key);
        if (it == m_buckets.constEnd()) {
            continue;
        }

        const QVector<Term>& bucket = it.value();
        for (const Term& candidate : bucket) {
            const QString compressedCandidate = compressRuns(candidate.text);
            if (std::abs(compressedCandidate.size() - compressedInput.size()) > maxDistance) {
                continue;
            }

            const int dist = editDistance(compressedInput, compressedCandidate, maxDistance);
            if (dist > maxDistance) {
                continue;
            }

            if (!best.has_value()
                || dist < best->editDistance
                || (dist == best->editDistance && candidate.docCount > best->docCount)) {
                best = Correction{candidate.text, dist, candidate.docCount};
            }
        }
    }

    return best;
}

bool TypoLexicon::contains(const QString& token) const
{
    if (!m_ready || token.size() < kMinTermLength) {
        return false;
    }

    const QString normalized = token.toLower();
    if (normalized.isEmpty()) {
        return false;
    }

    auto it = m_buckets.constFind(normalized.front());
    if (it == m_buckets.constEnd()) {
        return false;
    }

    const QVector<Term>& bucket = it.value();
    for (const Term& term : bucket) {
        if (term.text == normalized) {
            return true;
        }
    }
    return false;
}

void TypoLexicon::clear()
{
    m_buckets.clear();
    m_totalTerms = 0;
    m_ready = false;
}

int TypoLexicon::editDistance(const QString& a, const QString& b, int maxDist)
{
    const int aLen = a.size();
    const int bLen = b.size();

    if (a == b) {
        return 0;
    }
    if (aLen == 0) {
        return bLen <= maxDist ? bLen : maxDist + 1;
    }
    if (bLen == 0) {
        return aLen <= maxDist ? aLen : maxDist + 1;
    }
    if (std::abs(aLen - bLen) > maxDist) {
        return maxDist + 1;
    }

    // Optimal string alignment (restricted Damerau-Levenshtein):
    // deletion, insertion, substitution, AND adjacent transposition
    QVector<int> prevPrev(bLen + 1, 0);
    QVector<int> prev(bLen + 1);
    QVector<int> curr(bLen + 1);
    for (int j = 0; j <= bLen; ++j) {
        prev[j] = j;
    }

    for (int i = 1; i <= aLen; ++i) {
        curr[0] = i;
        int rowMin = curr[0];

        for (int j = 1; j <= bLen; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            const int deletion = prev[j] + 1;
            const int insertion = curr[j - 1] + 1;
            const int substitution = prev[j - 1] + cost;
            curr[j] = std::min({deletion, insertion, substitution});

            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                curr[j] = std::min(curr[j], prevPrev[j - 2] + 1);
            }

            rowMin = std::min(rowMin, curr[j]);
        }

        if (rowMin > maxDist) {
            return maxDist + 1;
        }

        prevPrev.swap(prev);
        prev.swap(curr);
    }

    return prev[bLen] <= maxDist ? prev[bLen] : maxDist + 1;
}

} // namespace bs
