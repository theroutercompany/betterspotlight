#include <QtTest/QtTest>

#include "ipc_test_utils.h"
#include "relevance_harness.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

#ifndef BS_RELEVANCE_SUITE_PATH
#define BS_RELEVANCE_SUITE_PATH ""
#endif

#ifndef BS_UI_SIM_QUERY_CORPUS_PATH
#define BS_UI_SIM_QUERY_CORPUS_PATH ""
#endif

namespace {

QString resolveSuitePath()
{
    return bs::test::resolveJsonFixturePath(
        QStringLiteral("BS_RELEVANCE_SUITE"),
        QString::fromUtf8(BS_RELEVANCE_SUITE_PATH),
        QStringLiteral("relevance/ui_sim_query_suite.json"));
}

QString resolveCorpusPath()
{
    return bs::test::resolveJsonFixturePath(
        QStringLiteral("BS_UI_SIM_QUERY_CORPUS"),
        QString::fromUtf8(BS_UI_SIM_QUERY_CORPUS_PATH),
        QStringLiteral("Fixtures/ui_sim_query_suite_v1/corpus.json"));
}

std::vector<bs::test::CorpusDocumentSpec> parseCorpus(const QJsonArray& docsArray)
{
    std::vector<bs::test::CorpusDocumentSpec> docs;
    docs.reserve(static_cast<size_t>(docsArray.size()));
    for (const QJsonValue& value : docsArray) {
        const QJsonObject obj = value.toObject();
        const QString relativePath = obj.value(QStringLiteral("relativePath")).toString();
        const QString content = obj.value(QStringLiteral("content")).toString();
        if (relativePath.isEmpty() || content.trimmed().isEmpty()) {
            continue;
        }
        docs.push_back({relativePath, content});
    }
    return docs;
}

} // namespace

class TestUiSimQuerySuite : public QObject {
    Q_OBJECT

private slots:
    void testRelevanceGateAgainstHermeticCorpus();
};

void TestUiSimQuerySuite::testRelevanceGateAgainstHermeticCorpus()
{
    const QString suitePath = resolveSuitePath();
    QVERIFY2(!suitePath.isEmpty(),
             "Relevance suite JSON not found (set BS_RELEVANCE_SUITE or provide compiled path)");

    const QString corpusPath = resolveCorpusPath();
    QVERIFY2(!corpusPath.isEmpty(),
             "Hermetic UI corpus JSON not found (set BS_UI_SIM_QUERY_CORPUS or provide compiled path)");

    QFile suiteFile(suitePath);
    QVERIFY2(suiteFile.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("Failed to open suite file: %1").arg(suitePath)));
    QJsonParseError suiteParseError;
    const QJsonDocument suiteDoc = QJsonDocument::fromJson(suiteFile.readAll(), &suiteParseError);
    QVERIFY2(suiteParseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("Invalid suite JSON (%1): %2")
                            .arg(suiteParseError.offset)
                            .arg(suiteParseError.errorString())));
    const QJsonObject suiteRoot = suiteDoc.object();

    QFile corpusFile(corpusPath);
    QVERIFY2(corpusFile.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("Failed to open corpus file: %1").arg(corpusPath)));
    QJsonParseError corpusParseError;
    const QJsonDocument corpusDoc =
        QJsonDocument::fromJson(corpusFile.readAll(), &corpusParseError);
    QVERIFY2(corpusParseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("Invalid corpus JSON (%1): %2")
                            .arg(corpusParseError.offset)
                            .arg(corpusParseError.errorString())));

    const std::vector<bs::test::RelevanceCase> cases =
        bs::test::parseRelevanceCases(suiteRoot.value(QStringLiteral("cases")).toArray());
    QVERIFY2(!cases.empty(), "No valid cases found in UI relevance suite JSON");

    const std::vector<bs::test::CorpusDocumentSpec> corpusDocs =
        parseCorpus(corpusDoc.object().value(QStringLiteral("documents")).toArray());
    QVERIFY2(!corpusDocs.empty(), "No corpus documents found in UI relevance corpus JSON");

    QSet<QString> corpusFileNames;
    for (const auto& doc : corpusDocs) {
        corpusFileNames.insert(QFileInfo(doc.relativePath).fileName().toLower());
    }

    QStringList invalidCases;
    for (const auto& testCase : cases) {
        if (!corpusFileNames.contains(testCase.expectedFileName.toLower())) {
            invalidCases.append(
                QStringLiteral("[%1] missing expected corpus file \"%2\"")
                    .arg(testCase.id, testCase.expectedFileName));
        }
    }
    if (!invalidCases.isEmpty()) {
        QFAIL(qPrintable(QStringLiteral("invalid_fixture_case:\n%1")
                             .arg(invalidCases.join(QStringLiteral("\n")))));
    }

    bs::test::HermeticQueryFixture fixture;
    QVERIFY2(fixture.isValid(), "Failed to create hermetic query fixture");
    QString error;
    QVERIFY2(fixture.seedGeneratedCorpus(corpusDocs, &error), qPrintable(error));
    QVERIFY2(fixture.startQueryService({}, &error), qPrintable(error));

    const bool needsVectors = std::any_of(
        cases.begin(), cases.end(), [](const bs::test::RelevanceCase& c) { return c.requiresVectors; });
    if (needsVectors) {
        QVERIFY2(fixture.ensureSemanticReady(&error), qPrintable(error));
    }

    const double gatePassRate = suiteRoot.value(QStringLiteral("gatePassRate")).toDouble(80.0);
    const QString gateMode = qEnvironmentVariable("BS_RELEVANCE_GATE_MODE").trimmed().toLower();
    const bool enforceGate = gateMode == QLatin1String("enforce");

    int passed = 0;
    int skipped = 0;
    QStringList failures;
    QJsonArray rankingMissDetails;

    for (const auto& testCase : cases) {
        if (testCase.category == QLatin1String("typo_strict")) {
            ++skipped;
            qInfo().noquote()
                << QStringLiteral("CASE %1 (%2) => SKIP (negative-path typo strict case)")
                       .arg(testCase.id, testCase.category);
            continue;
        }

        QJsonObject params;
        params[QStringLiteral("query")] = testCase.query;
        params[QStringLiteral("limit")] = std::max(3, testCase.topN);
        params[QStringLiteral("queryMode")] = testCase.mode;
        params[QStringLiteral("debug")] = true;

        const QJsonObject response = fixture.request(QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = bs::test::resultPayload(response);
        const QJsonArray ranked = result.value(QStringLiteral("results")).toArray();
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY2(!debugInfo.isEmpty(),
                 qPrintable(QStringLiteral("Missing debugInfo for %1").arg(testCase.id)));

        QStringList inspected;
        const bool ok = bs::test::containsExpectedFileInTopN(
            ranked, testCase.expectedFileName, testCase.topN, &inspected);
        if (ok) {
            ++passed;
        } else {
            const QString detail = QStringLiteral("[%1|%2] q=\"%3\" expect=\"%4\" topN=%5 saw=[%6]")
                                       .arg(testCase.id,
                                            testCase.category,
                                            testCase.query,
                                            testCase.expectedFileName,
                                            QString::number(testCase.topN),
                                            inspected.join(QStringLiteral(", ")));
            failures.append(detail);
            QJsonObject entry;
            entry[QStringLiteral("id")] = testCase.id;
            entry[QStringLiteral("category")] = testCase.category;
            entry[QStringLiteral("failureType")] = QStringLiteral("ranking_miss");
            entry[QStringLiteral("query")] = testCase.query;
            entry[QStringLiteral("expectedFileName")] = testCase.expectedFileName;
            entry[QStringLiteral("inspectedTopN")] = inspected.join(QStringLiteral(", "));
            rankingMissDetails.append(entry);
        }

        qInfo().noquote()
            << QStringLiteral("CASE %1 (%2) mode=%3 topN=%4 => %5")
                   .arg(testCase.id,
                        testCase.category,
                        testCase.mode,
                        QString::number(testCase.topN),
                        ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }

    const int total = static_cast<int>(cases.size()) - skipped;
    QVERIFY2(total > 0, "No evaluable cases found after skips");
    const double passRate = (100.0 * static_cast<double>(passed)) / static_cast<double>(total);
    const int requiredPasses = static_cast<int>(std::ceil((gatePassRate / 100.0) * total));

    qInfo().noquote() << QStringLiteral("Relevance gate summary: passed=%1/%2 passRate=%3%% required=%4%% (%5/%2)")
                             .arg(QString::number(passed),
                                  QString::number(total),
                                  QString::number(passRate, 'f', 2),
                                  QString::number(gatePassRate, 'f', 1),
                                  QString::number(requiredPasses));

    const QString reportPath = qEnvironmentVariable("BS_RELEVANCE_REPORT_PATH").trimmed();
    if (!reportPath.isEmpty()) {
        QJsonObject report;
        report[QStringLiteral("suitePath")] = suitePath;
        report[QStringLiteral("corpusPath")] = corpusPath;
        report[QStringLiteral("dbPath")] = fixture.dbPath();
        report[QStringLiteral("modelsDir")] = fixture.modelsDir();
        report[QStringLiteral("gateMode")] = enforceGate ? QStringLiteral("enforce")
                                                         : QStringLiteral("report_only");
        report[QStringLiteral("gatePassRate")] = gatePassRate;
        report[QStringLiteral("totalCases")] = total;
        report[QStringLiteral("passedCases")] = passed;
        report[QStringLiteral("passRate")] = passRate;
        report[QStringLiteral("requiredPasses")] = requiredPasses;
        report[QStringLiteral("skippedCases")] = skipped;
        report[QStringLiteral("semanticUnavailableCount")] = 0;
        report[QStringLiteral("rankingMisses")] = rankingMissDetails;
        report[QStringLiteral("semanticUnavailableCases")] = QJsonArray();
        report[QStringLiteral("fixtureMismatchCases")] = QJsonArray();
        QJsonArray failuresLegacy;
        for (const QString& line : failures) {
            failuresLegacy.append(line);
        }
        report[QStringLiteral("failures")] = failuresLegacy;
        report[QStringLiteral("timestampUtc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QSaveFile out(reportPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
            out.commit();
        }
    }

    for (const QString& line : failures) {
        qInfo().noquote() << line;
    }

    if (enforceGate) {
        QVERIFY2(passRate >= gatePassRate,
                 qPrintable(QStringLiteral("UI relevance gate failed: %1/%2 (%3%%) below gate %4%% (required %5)")
                                .arg(passed)
                                .arg(total)
                                .arg(QString::number(passRate, 'f', 2))
                                .arg(QString::number(gatePassRate, 'f', 1))
                                .arg(requiredPasses)));
    }
}

QTEST_MAIN(TestUiSimQuerySuite)
#include "test_ui_sim_query_suite.moc"
