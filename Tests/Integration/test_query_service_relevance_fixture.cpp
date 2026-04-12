#include <QtTest/QtTest>

#include "ipc_test_utils.h"
#include "relevance_harness.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

#ifndef BS_RELEVANCE_BASELINES_PATH
#define BS_RELEVANCE_BASELINES_PATH ""
#endif

namespace {

QString resolveBaselinesPath()
{
    return bs::test::resolveJsonFixturePath(
        QStringLiteral("BS_RELEVANCE_BASELINES"),
        QString::fromUtf8(BS_RELEVANCE_BASELINES_PATH),
        QStringLiteral("relevance/baselines.json"));
}

QString resolveFixtureRoot(const QString& fixtureId, const QString& baselinesPath)
{
    const QString fromEnv = qEnvironmentVariable("BS_RELEVANCE_FIXTURE_ROOT");
    if (!fromEnv.isEmpty() && QFileInfo(fromEnv).exists()) {
        return QDir::cleanPath(fromEnv);
    }

    if (!baselinesPath.isEmpty()) {
        const QString candidate = QDir(QFileInfo(baselinesPath).absolutePath())
                                      .filePath(QStringLiteral("../Fixtures/") + fixtureId);
        if (QFileInfo(candidate).exists()) {
            return QDir::cleanPath(candidate);
        }
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("../Tests/Fixtures/") + fixtureId),
        QDir(appDir).filePath(QStringLiteral("../../Tests/Fixtures/") + fixtureId),
        QDir(appDir).filePath(QStringLiteral("../../../Tests/Fixtures/") + fixtureId),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).exists()) {
            return QDir::cleanPath(candidate);
        }
    }

    return QString();
}

} // namespace

class TestQueryServiceRelevanceFixture : public QObject {
    Q_OBJECT

private slots:
    void testFixtureRelevanceGateViaIpc();
};

void TestQueryServiceRelevanceFixture::testFixtureRelevanceGateViaIpc()
{
    const QString baselinesPath = resolveBaselinesPath();
    QVERIFY2(!baselinesPath.isEmpty(),
             "baselines.json not found (set BS_RELEVANCE_BASELINES or compile definition)");

    QFile baselineFile(baselinesPath);
    QVERIFY2(baselineFile.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("Failed to open baselines: %1").arg(baselinesPath)));

    QJsonParseError parseError;
    const QJsonDocument baselineDoc = QJsonDocument::fromJson(baselineFile.readAll(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("Invalid baselines JSON (%1): %2")
                            .arg(parseError.offset)
                            .arg(parseError.errorString())));
    const QJsonObject root = baselineDoc.object();

    const QString fixtureId = root.value(QStringLiteral("fixtureId")).toString(
        QStringLiteral("standard_home_v1"));
    const QString fixtureRoot = resolveFixtureRoot(fixtureId, baselinesPath);
    QVERIFY2(!fixtureRoot.isEmpty(),
             qPrintable(QStringLiteral("Fixture root not found for fixtureId=%1").arg(fixtureId)));

    const std::vector<bs::test::RelevanceCase> cases =
        bs::test::parseRelevanceCases(root.value(QStringLiteral("cases")).toArray());
    QVERIFY2(!cases.empty(), "No valid cases in baselines.json");

    QSet<QString> fixtureFileNames;
    QDirIterator fixtureIt(fixtureRoot, QDir::Files, QDirIterator::Subdirectories);
    while (fixtureIt.hasNext()) {
        fixtureIt.next();
        fixtureFileNames.insert(fixtureIt.fileInfo().fileName().toLower());
    }

    QStringList invalidFixtureCases;
    for (const auto& testCase : cases) {
        if (!fixtureFileNames.contains(testCase.expectedFileName.toLower())) {
            invalidFixtureCases.append(
                QStringLiteral("[%1] missing expected fixture file \"%2\"")
                    .arg(testCase.id, testCase.expectedFileName));
        }
    }
    if (!invalidFixtureCases.isEmpty()) {
        QFAIL(qPrintable(QStringLiteral("invalid_fixture_case:\n%1")
                             .arg(invalidFixtureCases.join(QStringLiteral("\n")))));
    }

    bs::test::HermeticQueryFixture fixture;
    QVERIFY2(fixture.isValid(), "Failed to create hermetic query fixture");
    QString error;
    QVERIFY2(fixture.seedFixtureTreeUnderDocuments(fixtureRoot, &error), qPrintable(error));
    QVERIFY2(fixture.startQueryService({}, &error), qPrintable(error));

    const bool needsVectors = std::any_of(
        cases.begin(), cases.end(), [](const bs::test::RelevanceCase& c) { return c.requiresVectors; });
    if (needsVectors) {
        QVERIFY2(fixture.ensureSemanticReady(&error), qPrintable(error));
    }

    int passed = 0;
    int skipped = 0;
    QStringList failures;
    QJsonArray rankingMissDetails;
    const QString gateMode = qEnvironmentVariable("BS_RELEVANCE_GATE_MODE").trimmed().toLower();
    const bool enforceGate = gateMode.isEmpty() || gateMode == QLatin1String("enforce");

    for (const auto& testCase : cases) {
        if (testCase.category == QLatin1String("typo_strict")) {
            ++skipped;
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

        QStringList inspected;
        const bool ok = bs::test::containsExpectedFileInTopN(
            ranked, testCase.expectedFileName, testCase.topN, &inspected);
        if (ok) {
            ++passed;
            continue;
        }

        failures.append(QStringLiteral("[%1|%2] q=\"%3\" expect=\"%4\" topN=%5 saw=[%6]")
                            .arg(testCase.id,
                                 testCase.category,
                                 testCase.query,
                                 testCase.expectedFileName,
                                 QString::number(testCase.topN),
                                 inspected.join(QStringLiteral(", "))));
        QJsonObject entry;
        entry[QStringLiteral("id")] = testCase.id;
        entry[QStringLiteral("category")] = testCase.category;
        entry[QStringLiteral("failureType")] = QStringLiteral("ranking_miss");
        entry[QStringLiteral("query")] = testCase.query;
        entry[QStringLiteral("expectedFileName")] = testCase.expectedFileName;
        entry[QStringLiteral("inspectedTopN")] = inspected.join(QStringLiteral(", "));
        rankingMissDetails.append(entry);
    }

    const int total = static_cast<int>(cases.size()) - skipped;
    QVERIFY2(total > 0, "No evaluated baseline cases after skips");
    const double passRate = (100.0 * static_cast<double>(passed)) / static_cast<double>(total);
    const double gatePassRate = root.value(QStringLiteral("gatePassRate")).toDouble(90.0);
    const int requiredPasses = static_cast<int>(std::ceil((gatePassRate / 100.0) * total));

    const QString reportPath = qEnvironmentVariable("BS_RELEVANCE_FIXTURE_REPORT_PATH").trimmed();
    if (!reportPath.isEmpty()) {
        QJsonObject report;
        report[QStringLiteral("baselinesPath")] = baselinesPath;
        report[QStringLiteral("fixtureRoot")] = fixtureRoot;
        report[QStringLiteral("dbPath")] = fixture.dbPath();
        report[QStringLiteral("modelsDir")] = fixture.modelsDir();
        report[QStringLiteral("gatePassRate")] = gatePassRate;
        report[QStringLiteral("gateMode")] = enforceGate
            ? QStringLiteral("enforce")
            : QStringLiteral("report_only");
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
                 qPrintable(QStringLiteral("Fixture relevance gate failed: %1/%2 (%3%%) below gate %4%% (required %5)")
                                .arg(passed)
                                .arg(total)
                                .arg(QString::number(passRate, 'f', 2))
                                .arg(QString::number(gatePassRate, 'f', 1))
                                .arg(requiredPasses)));
    }
}

QTEST_MAIN(TestQueryServiceRelevanceFixture)
#include "test_query_service_relevance_fixture.moc"
