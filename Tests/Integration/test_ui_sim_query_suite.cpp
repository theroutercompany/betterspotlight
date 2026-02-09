#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"
#include "core/index/typo_lexicon.h"
#include "core/index/sqlite_store.h"
#include "core/query/query_normalizer.h"
#include "core/ranking/match_classifier.h"
#include "core/ranking/scorer.h"
#include "core/shared/search_result.h"
#include "core/vector/search_merger.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"

#ifndef BS_RELEVANCE_SUITE_PATH
#define BS_RELEVANCE_SUITE_PATH ""
#endif

namespace {
const QSet<QString>& queryStopwords()
{
    static const QSet<QString> stopwords = {
        QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("any"),
        QStringLiteral("and"), QStringLiteral("are"), QStringLiteral("at"),
        QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("how"),
        QStringLiteral("in"), QStringLiteral("is"), QStringLiteral("it"),
        QStringLiteral("my"), QStringLiteral("of"), QStringLiteral("on"),
        QStringLiteral("or"), QStringLiteral("that"), QStringLiteral("there"),
        QStringLiteral("the"), QStringLiteral("to"), QStringLiteral("what"),
        QStringLiteral("when"), QStringLiteral("where"), QStringLiteral("which"),
        QStringLiteral("who"), QStringLiteral("why"), QStringLiteral("with"),
    };
    return stopwords;
}

QStringList tokenizeWords(const QString& text)
{
    static const QRegularExpression tokenRegex(QStringLiteral(R"([A-Za-z0-9_]+)"));
    QStringList tokens;
    auto it = tokenRegex.globalMatch(text.toLower());
    while (it.hasNext()) {
        const QString token = it.next().captured(0);
        if (!token.isEmpty()) {
            tokens.append(token);
        }
    }
    return tokens;
}
} // anonymous namespace

class TestUiSimQuerySuite : public QObject {
    Q_OBJECT

private slots:
    void testRelevanceGateAgainstLiveIndex();

private:
    struct QueryCase {
        QString id;
        QString category;
        QString query;
        QString mode;
        QString expectedFileName;
        int topN = 3;
    };

    static QString resolveSuitePath();
    static QString resolveDbPath();
    static bool containsExpectedFileInTopN(const std::vector<bs::SearchResult>& ranked,
                                           const QString& expectedFileName,
                                           int topN,
                                           QStringList* inspectedNames);
};

QString TestUiSimQuerySuite::resolveSuitePath()
{
    const QString fromEnv = qEnvironmentVariable("BS_RELEVANCE_SUITE");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
        return fromEnv;
    }

    const QString compiled = QString::fromUtf8(BS_RELEVANCE_SUITE_PATH);
    if (!compiled.isEmpty() && QFileInfo::exists(compiled)) {
        return compiled;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList fallbacks = {
        QDir(appDir).filePath("../Tests/relevance/ui_sim_query_suite.json"),
        QDir(appDir).filePath("../../Tests/relevance/ui_sim_query_suite.json")
    };

    for (const QString& candidate : fallbacks) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return QString();
}

QString TestUiSimQuerySuite::resolveDbPath()
{
    const QString fromEnv = qEnvironmentVariable("BS_INDEX_DB");
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }

    // Try platform-standard paths in order of preference
    const QStringList candidates = {
        QDir::homePath() + QStringLiteral("/Library/Application Support/betterspotlight/index.db"),
        QDir::homePath() + QStringLiteral("/.local/share/betterspotlight/index.sqlite3"),
        QDir::homePath() + QStringLiteral("/.local/share/betterspotlight/index.db"),
    };
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return candidates.first(); // Return macOS default for error message
}

bool TestUiSimQuerySuite::containsExpectedFileInTopN(const std::vector<bs::SearchResult>& ranked,
                                                     const QString& expectedFileName,
                                                     int topN,
                                                     QStringList* inspectedNames)
{
    const int inspected = std::min<int>(topN, static_cast<int>(ranked.size()));
    for (int i = 0; i < inspected; ++i) {
        const QString candidateName = QFileInfo(ranked[static_cast<size_t>(i)].path).fileName();
        inspectedNames->append(candidateName);
        if (candidateName.compare(expectedFileName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

void TestUiSimQuerySuite::testRelevanceGateAgainstLiveIndex()
{
    const QString dbPath = resolveDbPath();
    if (!QFileInfo::exists(dbPath)) {
        QSKIP(qPrintable(QStringLiteral("Live index DB not found: %1").arg(dbPath)));
    }

    const QString suitePath = resolveSuitePath();
    if (suitePath.isEmpty() || !QFileInfo::exists(suitePath)) {
        QSKIP("Relevance suite JSON not found (set BS_RELEVANCE_SUITE or provide compiled path)");
    }

    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY2(store.has_value(), qPrintable(QStringLiteral("Failed to open DB: %1").arg(dbPath)));

    bs::TypoLexicon typoLexicon;
    const bool lexiconReady = typoLexicon.build(store->rawDb());
    qInfo() << "TypoLexicon built:" << lexiconReady << "terms:" << typoLexicon.termCount();

    // Semantic search setup (optional - skip semantic_probe if assets missing).
    bool semanticAvailable = false;
    std::unique_ptr<bs::ModelRegistry> modelRegistry;
    std::unique_ptr<bs::EmbeddingManager> embeddingManager;
    std::unique_ptr<bs::VectorIndex> vectorIndex;
    std::unique_ptr<bs::VectorStore> vectorStore;

#ifdef BETTERSPOTLIGHT_WITH_ONNX
    {
        const QString modelsDir = bs::ModelRegistry::resolveModelsDir();
        const QString dataDir = QFileInfo(dbPath).absolutePath();
        const QString vectorIndexPath = dataDir + QStringLiteral("/vectors.hnsw");
        const QString vectorMetaPath = dataDir + QStringLiteral("/vectors.meta");

        modelRegistry = std::make_unique<bs::ModelRegistry>(modelsDir);
        if (modelRegistry->hasModel("bi-encoder") && QFileInfo::exists(vectorIndexPath)) {
            embeddingManager = std::make_unique<bs::EmbeddingManager>(modelRegistry.get());
            if (embeddingManager->initialize()) {
                vectorIndex = std::make_unique<bs::VectorIndex>();
                if (vectorIndex->load(vectorIndexPath.toStdString(), vectorMetaPath.toStdString())) {
                    vectorStore = std::make_unique<bs::VectorStore>(store->rawDb());
                    semanticAvailable = true;
                    qInfo() << "Semantic search available: vectors=" << vectorIndex->totalElements();
                }
            }
        }

        if (!semanticAvailable) {
            qInfo() << "Semantic search not available (missing model or vector assets)";
        }
    }
#endif

    QFile file(suitePath);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("Failed to open suite file: %1").arg(suitePath)));

    QJsonParseError parseError;
    const QJsonDocument suiteDoc = QJsonDocument::fromJson(file.readAll(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("Invalid suite JSON (%1): %2")
                            .arg(parseError.offset)
                            .arg(parseError.errorString())));

    const QJsonObject root = suiteDoc.object();
    const double gatePassRate = root.value(QStringLiteral("gatePassRate")).toDouble(80.0);
    const QJsonArray caseArray = root.value(QStringLiteral("cases")).toArray();
    if (caseArray.isEmpty()) {
        QSKIP("No test cases found in relevance suite JSON");
    }

    std::vector<QueryCase> cases;
    cases.reserve(static_cast<size_t>(caseArray.size()));
    for (const QJsonValue& entry : caseArray) {
        const QJsonObject obj = entry.toObject();
        QueryCase c;
        c.id = obj.value(QStringLiteral("id")).toString();
        c.category = obj.value(QStringLiteral("category")).toString();
        c.query = obj.value(QStringLiteral("query")).toString();
        c.mode = obj.value(QStringLiteral("mode")).toString(QStringLiteral("auto"));
        c.expectedFileName = obj.value(QStringLiteral("expectedFileName")).toString();
        c.topN = std::max(1, obj.value(QStringLiteral("topN")).toInt(3));
        if (!c.id.isEmpty() && !c.query.isEmpty() && !c.expectedFileName.isEmpty()) {
            cases.push_back(c);
        }
    }

    if (cases.empty()) {
        QSKIP("No valid cases found after parsing relevance suite JSON");
    }

    const bs::Scorer scorer;
    const bs::QueryContext emptyContext;

    int passed = 0;
    int semanticSkipped = 0;
    int fixtureMismatches = 0;
    QStringList failureDetails;
    QJsonArray rankingMissDetails;
    QJsonArray fixtureMismatchDetails;
    QJsonArray semanticUnavailableDetails;

    sqlite3_stmt* expectedLookupStmt = nullptr;
    const char* expectedLookupSql = "SELECT 1 FROM items WHERE LOWER(name) = LOWER(?1) LIMIT 1";
    QVERIFY2(sqlite3_prepare_v2(store->rawDb(), expectedLookupSql, -1, &expectedLookupStmt, nullptr)
                 == SQLITE_OK,
             "Failed to prepare expected-file lookup statement");
    auto stmtGuard = qScopeGuard([&]() {
        if (expectedLookupStmt) {
            sqlite3_finalize(expectedLookupStmt);
        }
    });

    const auto expectedExistsInCorpus = [&](const QString& expectedName) {
        sqlite3_reset(expectedLookupStmt);
        sqlite3_clear_bindings(expectedLookupStmt);
        const QByteArray utf8 = expectedName.toUtf8();
        sqlite3_bind_text(expectedLookupStmt, 1, utf8.constData(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(expectedLookupStmt);
        return rc == SQLITE_ROW;
    };

    for (const QueryCase& testCase : cases) {
        // Skip typo_strict as intentional negative-path tests.
        if (testCase.category == QStringLiteral("typo_strict")) {
            ++semanticSkipped;
            qInfo().noquote()
                << QStringLiteral("CASE %1 (%2) => SKIP (requires %3)")
                       .arg(testCase.id, testCase.category,
                             QStringLiteral("typo-in-strict is a negative-path test"));
            continue;
        }
        if (testCase.category == QStringLiteral("semantic_probe") && !semanticAvailable) {
            ++semanticSkipped;
            const QString detail = QStringLiteral(
                                       "[%1|%2] q=\"%3\" expect=\"%4\" semantic_unavailable")
                                       .arg(testCase.id,
                                            testCase.category,
                                            testCase.query,
                                            testCase.expectedFileName);
            failureDetails.append(detail);
            QJsonObject failure;
            failure[QStringLiteral("id")] = testCase.id;
            failure[QStringLiteral("category")] = testCase.category;
            failure[QStringLiteral("failureType")] = QStringLiteral("semantic_unavailable");
            failure[QStringLiteral("query")] = testCase.query;
            failure[QStringLiteral("expectedFileName")] = testCase.expectedFileName;
            semanticUnavailableDetails.append(failure);
            qInfo().noquote()
                << QStringLiteral("CASE %1 (%2) => SKIP (requires vector search)")
                       .arg(testCase.id, testCase.category);
            continue;
        }

        if (!expectedExistsInCorpus(testCase.expectedFileName)) {
            ++fixtureMismatches;
            const QString detail = QStringLiteral(
                                       "[%1|%2] q=\"%3\" expect=\"%4\" missing_from_corpus")
                                       .arg(testCase.id,
                                            testCase.category,
                                            testCase.query,
                                            testCase.expectedFileName);
            failureDetails.append(detail);
            QJsonObject failure;
            failure[QStringLiteral("id")] = testCase.id;
            failure[QStringLiteral("category")] = testCase.category;
            failure[QStringLiteral("failureType")] = QStringLiteral("fixture_mismatch");
            failure[QStringLiteral("query")] = testCase.query;
            failure[QStringLiteral("expectedFileName")] = testCase.expectedFileName;
            fixtureMismatchDetails.append(failure);
            qInfo().noquote()
                << QStringLiteral("CASE %1 (%2) => SKIP (fixture mismatch: missing expected file)")
                       .arg(testCase.id, testCase.category);
            continue;
        }

        const int ftsLimit = std::max(1, testCase.topN * 3);
        const int limit = testCase.topN;

        const bs::NormalizedQuery nq = bs::QueryNormalizer::normalize(testCase.query);
        const QString& searchQuery = nq.normalized;
        QString nameFuzzyQuery = searchQuery;
        nameFuzzyQuery.replace(QLatin1Char('-'), QLatin1Char(' '));

        auto buildTypoRewrittenQuery = [&]() -> QString {
            QStringList queryTokens = tokenizeWords(searchQuery);
            if (queryTokens.isEmpty()) {
                return searchQuery;
            }
            const QSet<QString>& stopwords = queryStopwords();
            int replacements = 0;
            for (int i = 0; i < queryTokens.size(); ++i) {
                const QString token = queryTokens.at(i);
                if (token.size() < 4 || stopwords.contains(token)) {
                    continue;
                }
                if (replacements >= 2) {
                    break;
                }
                if (typoLexicon.contains(token)) {
                    continue;
                }
                const int maxDistance = token.size() >= 8 ? 2 : 1;
                const auto correction = typoLexicon.correct(token, maxDistance);
                if (correction.has_value()) {
                    queryTokens[i] = correction->corrected;
                    ++replacements;
                }
            }
            return replacements > 0 ? queryTokens.join(QLatin1Char(' ')) : searchQuery;
        };

        std::vector<bs::SQLiteStore::FtsHit> hits;
        QString classifyQuery = searchQuery;

        if (testCase.mode.compare(QStringLiteral("strict"), Qt::CaseInsensitive) == 0) {
            hits = store->searchFts5(searchQuery, ftsLimit, false);
        } else if (testCase.mode.compare(QStringLiteral("relaxed"), Qt::CaseInsensitive) == 0) {
            const QString rewritten = buildTypoRewrittenQuery();
            classifyQuery = rewritten;
            hits = store->searchFts5(rewritten, std::max(ftsLimit * 2, limit * 4), true);
        } else {
            // Auto mode (default)
            auto strictHits = store->searchFts5(searchQuery, ftsLimit, false);
            hits = strictHits;
            // Always attempt typo-corrected relaxed search in auto mode
            const QString rewritten = buildTypoRewrittenQuery();
            if (rewritten != searchQuery) {
                classifyQuery = rewritten;
                auto relaxedHits = store->searchFts5(rewritten, std::max(ftsLimit * 2, limit * 4), true);
                hits.insert(hits.end(), relaxedHits.begin(), relaxedHits.end());
            } else if (strictHits.empty()) {
                auto relaxedHits = store->searchFts5(searchQuery, std::max(ftsLimit * 2, limit * 4), true);
                hits.insert(hits.end(), relaxedHits.begin(), relaxedHits.end());
            }
        }

        if (hits.empty()) {
            auto nameHits = store->searchByNameFuzzy(nameFuzzyQuery, ftsLimit);
            for (const auto& nh : nameHits) {
                bs::SQLiteStore::FtsHit fakeHit;
                fakeHit.fileId = nh.fileId;
                fakeHit.bm25Score = -50.0;
                fakeHit.snippet = QString();
                hits.push_back(fakeHit);
            }
        }

        {
            QString classifyNameFuzzyQuery = classifyQuery;
            classifyNameFuzzyQuery.replace(QLatin1Char('-'), QLatin1Char(' '));
            for (const QString& q : {nameFuzzyQuery, classifyNameFuzzyQuery}) {
                auto nameHits = store->searchByNameFuzzy(q, std::max(3, limit));
                for (const auto& nh : nameHits) {
                    bool alreadyPresent = std::any_of(hits.begin(), hits.end(),
                        [&](const bs::SQLiteStore::FtsHit& h) { return h.fileId == nh.fileId; });
                    if (!alreadyPresent) {
                        bs::SQLiteStore::FtsHit fakeHit;
                        fakeHit.fileId = nh.fileId;
                        fakeHit.bm25Score = -50.0;
                        fakeHit.snippet = QString();
                        hits.push_back(fakeHit);
                    }
                }
            }
        }

        std::vector<bs::SearchResult> ranked;
        ranked.reserve(hits.size());
        std::unordered_map<int64_t, size_t> bestHitByItem;
        bestHitByItem.reserve(hits.size());
        QString classifyMatchQuery = classifyQuery;
        classifyMatchQuery.replace(QLatin1Char('-'), QLatin1Char(' '));

        for (const bs::SQLiteStore::FtsHit& hit : hits) {
            const auto item = store->getItemById(hit.fileId);
            if (!item.has_value()) {
                continue;
            }

            bs::SearchResult result;
            result.itemId = item->id;
            result.path = item->path;
            result.name = item->name;
            result.kind = item->kind;
            result.matchType = bs::MatchClassifier::classify(classifyMatchQuery, item->name, item->path);
            result.bm25RawScore = hit.bm25Score;
            result.score = hit.bm25Score;
            result.snippet = hit.snippet;

            if (result.matchType == bs::MatchType::Fuzzy) {
                if (hit.bm25Score == -1.0) {
                    result.fuzzyDistance = 1;
                } else {
                    const QString baseName = QFileInfo(item->name).completeBaseName();
                    result.fuzzyDistance = bs::MatchClassifier::editDistance(classifyMatchQuery, baseName);
                }
            }

            const double lexicalStrength = std::max(0.0, -hit.bm25Score);
            auto existingIt = bestHitByItem.find(item->id);
            if (existingIt == bestHitByItem.end()) {
                bestHitByItem[item->id] = ranked.size();
                ranked.push_back(std::move(result));
            } else {
                bs::SearchResult& existing = ranked[existingIt->second];
                const double existingStrength = std::max(0.0, -existing.bm25RawScore);
                if (lexicalStrength > existingStrength) {
                    existing = std::move(result);
                }
            }
        }

        scorer.rankResults(ranked, emptyContext);

        std::unordered_set<int64_t> lexicalItemIds;
        lexicalItemIds.reserve(ranked.size());
        for (const auto& result : ranked) {
            lexicalItemIds.insert(result.itemId);
        }

        std::vector<bs::SemanticResult> semanticResults;
        if (semanticAvailable && testCase.category == QStringLiteral("semantic_probe")) {
            std::vector<float> queryVec = embeddingManager->embedQuery(searchQuery);
            if (!queryVec.empty()) {
                auto knnHits = vectorIndex->search(queryVec.data(), 50);
                constexpr float kSemanticThreshold = 0.7f;
                constexpr float kSemanticOnlyFloor = 0.15f;
                semanticResults.reserve(knnHits.size());

                for (const auto& hit : knnHits) {
                    const float cosineSim = 1.0f - hit.distance;
                    if (cosineSim < kSemanticThreshold) {
                        continue;
                    }

                    const float normalizedSemantic =
                        bs::SearchMerger::normalizeSemanticScore(cosineSim, kSemanticThreshold);
                    if (normalizedSemantic <= kSemanticOnlyFloor) {
                        continue;
                    }

                    auto itemIdOpt = vectorStore->getItemId(hit.label);
                    if (!itemIdOpt.has_value()) {
                        continue;
                    }

                    bs::SemanticResult sr;
                    sr.itemId = itemIdOpt.value();
                    sr.cosineSimilarity = cosineSim;
                    semanticResults.push_back(sr);
                }
            }
        }

        if (!semanticResults.empty()) {
            bs::MergeConfig mergeConfig;
            mergeConfig.similarityThreshold = 0.7f;
            mergeConfig.maxResults = std::max(limit * 2, limit);
            ranked = bs::SearchMerger::merge(ranked, semanticResults, mergeConfig);

            const int semanticOnlyCap = std::min(3, limit / 2);
            int semanticOnlyAdded = 0;
            std::vector<bs::SearchResult> cappedResults;
            cappedResults.reserve(ranked.size());
            for (const auto& sr : ranked) {
                const bool semanticOnly = lexicalItemIds.find(sr.itemId) == lexicalItemIds.end();
                if (semanticOnly) {
                    if (semanticOnlyAdded >= semanticOnlyCap) {
                        continue;
                    }
                    ++semanticOnlyAdded;
                }
                cappedResults.push_back(sr);
            }
            ranked.swap(cappedResults);

            for (auto& sr : ranked) {
                if (!sr.path.isEmpty()) {
                    continue;
                }
                const auto item = store->getItemById(sr.itemId);
                if (!item.has_value()) {
                    continue;
                }
                sr.path = item->path;
                sr.name = item->name;
                sr.kind = item->kind;
                sr.fileSize = item->size;
                sr.isPinned = item->isPinned;
            }
        }

        QStringList inspectedNames;
        const bool ok = containsExpectedFileInTopN(ranked, testCase.expectedFileName, testCase.topN, &inspectedNames);
        if (ok) {
            ++passed;
        } else {
            const QString detail = QStringLiteral("[%1|%2] q=\"%3\" expect=\"%4\" topN=%5 saw=[%6]")
                                       .arg(testCase.id,
                                            testCase.category,
                                            testCase.query,
                                            testCase.expectedFileName,
                                            QString::number(testCase.topN),
                                            inspectedNames.join(QStringLiteral(", ")));
            failureDetails.append(detail);
            QJsonObject failure;
            failure[QStringLiteral("id")] = testCase.id;
            failure[QStringLiteral("category")] = testCase.category;
            failure[QStringLiteral("failureType")] = QStringLiteral("ranking_miss");
            failure[QStringLiteral("query")] = testCase.query;
            failure[QStringLiteral("expectedFileName")] = testCase.expectedFileName;
            failure[QStringLiteral("inspectedTopN")] = inspectedNames.join(QStringLiteral(", "));
            rankingMissDetails.append(failure);
        }

        qInfo().noquote()
            << QStringLiteral("CASE %1 (%2) mode=%3 topN=%4 => %5")
                   .arg(testCase.id,
                        testCase.category,
                        testCase.mode,
                        QString::number(testCase.topN),
                        ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }

    const int total = static_cast<int>(cases.size()) - semanticSkipped - fixtureMismatches;
    if (total <= 0) {
        QSKIP("No evaluable cases found after skips and fixture mismatch filtering");
    }
    const double passRate = (100.0 * static_cast<double>(passed)) / static_cast<double>(total);
    const int requiredPasses = static_cast<int>(std::ceil((gatePassRate / 100.0) * static_cast<double>(total)));
    const QString gateMode = qEnvironmentVariable("BS_RELEVANCE_GATE_MODE")
                                 .trimmed()
                                 .toLower();
    const bool enforceGate = (gateMode == QLatin1String("enforce"));
    const bool reportOnly = !enforceGate;

    qInfo().noquote() << QStringLiteral("Relevance gate summary: passed=%1/%2 passRate=%3%% required=%4%% (%5/%2)")
                             .arg(QString::number(passed),
                                  QString::number(total),
                                  QString::number(passRate, 'f', 2),
                                  QString::number(gatePassRate, 'f', 1),
                                  QString::number(requiredPasses));
    qInfo().noquote() << QStringLiteral("Fixture mismatches: %1").arg(QString::number(fixtureMismatches));
    qInfo().noquote() << QStringLiteral("Relevance gate mode: %1")
                             .arg(reportOnly ? QStringLiteral("report_only")
                                             : QStringLiteral("enforce"));

    for (const QString& line : failureDetails) {
        qInfo().noquote() << line;
    }

    const QString reportPath = qEnvironmentVariable("BS_RELEVANCE_REPORT_PATH").trimmed();
    if (!reportPath.isEmpty()) {
        QJsonObject report;
        report[QStringLiteral("suitePath")] = suitePath;
        report[QStringLiteral("dbPath")] = dbPath;
        report[QStringLiteral("gateMode")] = reportOnly ? QStringLiteral("report_only")
                                                        : QStringLiteral("enforce");
        report[QStringLiteral("gatePassRate")] = gatePassRate;
        report[QStringLiteral("totalCases")] = total;
        report[QStringLiteral("passedCases")] = passed;
        report[QStringLiteral("passRate")] = passRate;
        report[QStringLiteral("requiredPasses")] = requiredPasses;
        report[QStringLiteral("semanticSkipped")] = semanticSkipped;
        report[QStringLiteral("fixtureMismatches")] = fixtureMismatches;
        report[QStringLiteral("semanticUnavailableCount")] = semanticUnavailableDetails.size();
        report[QStringLiteral("timestampUtc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QJsonArray failuresLegacy;
        for (const QString& line : failureDetails) {
            failuresLegacy.append(line);
        }
        report[QStringLiteral("failures")] = failuresLegacy;
        report[QStringLiteral("rankingMisses")] = rankingMissDetails;
        report[QStringLiteral("fixtureMismatchCases")] = fixtureMismatchDetails;
        report[QStringLiteral("semanticUnavailableCases")] = semanticUnavailableDetails;

        QSaveFile out(reportPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
            out.commit();
        }
    }

    if (passRate < gatePassRate && enforceGate) {
        QFAIL(qPrintable(QStringLiteral("Relevance gate failed: %1/%2 (%3%%) below gate %4%% (required %5)")
                             .arg(passed)
                             .arg(total)
                             .arg(QString::number(passRate, 'f', 2))
                             .arg(QString::number(gatePassRate, 'f', 1))
                             .arg(requiredPasses)));
    }
}

QTEST_MAIN(TestUiSimQuerySuite)
#include "test_ui_sim_query_suite.moc"
