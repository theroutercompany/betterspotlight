#include <QtTest/QtTest>

#include "core/ipc/message.h"
#include "core/ipc/service_base.h"
#include "core/ipc/socket_client.h"
#include "core/index/sqlite_store.h"
#include "core/shared/chunk.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <optional>

#ifndef BS_RELEVANCE_BASELINES_PATH
#define BS_RELEVANCE_BASELINES_PATH ""
#endif

namespace {

struct QueryCase {
    QString id;
    QString category;
    QString query;
    QString mode;
    QString expectedFileName;
    int topN = 3;
    bool semanticRequired = false;
    bool requiresVectors = false;
    QString notes;
};

QString findQueryBinary()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString name = QStringLiteral("betterspotlight-query");
    const QStringList candidates = {
        QDir(appDir).filePath(name),
        QDir(appDir).filePath(QStringLiteral("../src/services/query/") + name),
        QDir(appDir).filePath(QStringLiteral("../../src/services/query/") + name),
        QDir(appDir).filePath(QStringLiteral("../../../src/services/query/") + name),
        QDir(appDir).filePath(QStringLiteral("../bin/") + name),
        QDir(appDir).filePath(QStringLiteral("../../bin/") + name),
    };

    for (const QString& candidate : candidates) {
        QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) {
            return info.canonicalFilePath();
        }
    }

    return QStandardPaths::findExecutable(name);
}

bool waitForQueryConnection(bs::SocketClient& client, const QString& socketPath, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (client.connectToServer(socketPath, 100)) {
            return true;
        }
        QTest::qWait(25);
    }
    return false;
}

QString resolveBaselinesPath()
{
    const QString fromEnv = qEnvironmentVariable("BS_RELEVANCE_BASELINES");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
        return fromEnv;
    }

    const QString compiled = QString::fromUtf8(BS_RELEVANCE_BASELINES_PATH);
    if (!compiled.isEmpty() && QFileInfo::exists(compiled)) {
        return compiled;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath("../Tests/relevance/baselines.json"),
        QDir(appDir).filePath("../../Tests/relevance/baselines.json"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
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
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).exists()) {
            return QDir::cleanPath(candidate);
        }
    }
    return QString();
}

QString tokenizedName(const QString& fileName)
{
    QString out = fileName;
    out.replace(QLatin1Char('-'), QLatin1Char(' '));
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    out.replace(QLatin1Char('.'), QLatin1Char(' '));
    return out.simplified().toLower();
}

QString syntheticContentForFile(const QString& sourcePath)
{
    QFileInfo info(sourcePath);
    QString content = tokenizedName(info.fileName());
    content += QStringLiteral(" ");
    content += tokenizedName(info.completeBaseName());

    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return content;
    }
    const QByteArray bytes = file.read(8192);
    file.close();
    if (bytes.contains('\0')) {
        return content;
    }

    QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.isEmpty() && !bytes.isEmpty()) {
        decoded = QString::fromLatin1(bytes.constData(), bytes.size());
    }
    if (!decoded.isEmpty()) {
        content += QStringLiteral("\n");
        content += decoded.simplified();
    }
    return content;
}

bs::ItemKind classifyKind(const QString& extensionLower)
{
    if (extensionLower == QLatin1String("pdf")) {
        return bs::ItemKind::Pdf;
    }
    if (extensionLower == QLatin1String("md") || extensionLower == QLatin1String("markdown")) {
        return bs::ItemKind::Markdown;
    }
    if (extensionLower == QLatin1String("png") || extensionLower == QLatin1String("jpg")
        || extensionLower == QLatin1String("jpeg") || extensionLower == QLatin1String("webp")) {
        return bs::ItemKind::Image;
    }
    if (extensionLower == QLatin1String("mp3") || extensionLower == QLatin1String("mp4")
        || extensionLower == QLatin1String("mov")) {
        return bs::ItemKind::Binary;
    }
    if (extensionLower == QLatin1String("cpp") || extensionLower == QLatin1String("h")
        || extensionLower == QLatin1String("py") || extensionLower == QLatin1String("ts")
        || extensionLower == QLatin1String("js") || extensionLower == QLatin1String("go")
        || extensionLower == QLatin1String("rs")) {
        return bs::ItemKind::Code;
    }
    return bs::ItemKind::Text;
}

QJsonObject sendOrFail(bs::SocketClient& client,
                       const QString& method,
                       const QJsonObject& params = {})
{
    auto response = client.sendRequest(method, params, 3000);
    if (!response.has_value()) {
        return QJsonObject();
    }
    return response.value();
}

bool containsExpectedFileInTopN(const QJsonArray& ranked,
                                const QString& expectedFileName,
                                int topN,
                                QStringList* inspectedNames)
{
    const int inspected = std::min<int>(topN, ranked.size());
    for (int i = 0; i < inspected; ++i) {
        const QString candidateName =
            QFileInfo(ranked.at(i).toObject().value(QStringLiteral("path")).toString()).fileName();
        inspectedNames->append(candidateName);
        if (candidateName.compare(expectedFileName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
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

    std::vector<QueryCase> cases;
    const QJsonArray caseArray = root.value(QStringLiteral("cases")).toArray();
    for (const QJsonValue& value : caseArray) {
        const QJsonObject obj = value.toObject();
        QueryCase c;
        c.id = obj.value(QStringLiteral("id")).toString();
        c.category = obj.value(QStringLiteral("category")).toString();
        c.query = obj.value(QStringLiteral("query")).toString();
        c.mode = obj.value(QStringLiteral("mode")).toString(QStringLiteral("auto"));
        c.expectedFileName = obj.value(QStringLiteral("expectedFileName")).toString();
        c.topN = std::max(1, obj.value(QStringLiteral("topN")).toInt(3));
        c.semanticRequired = obj.value(QStringLiteral("semanticRequired")).toBool(false);
        c.requiresVectors = obj.value(QStringLiteral("requiresVectors")).toBool(c.semanticRequired);
        c.notes = obj.value(QStringLiteral("notes")).toString();
        if (!c.id.isEmpty() && !c.query.isEmpty() && !c.expectedFileName.isEmpty()) {
            cases.push_back(c);
        }
    }
    QVERIFY2(!cases.empty(), "No valid cases in baselines.json");

    QTemporaryDir tempHome;
    QVERIFY2(tempHome.isValid(), "Failed to create temporary HOME directory");

    const QString dataDir = QDir(tempHome.path())
                                .filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to initialize fixture SQLite store");
    bs::SQLiteStore store = std::move(storeOpt.value());

    // Seed fixture files under HOME/Documents so consumer prefilter remains effective.
    const QString targetRoot = QDir(tempHome.path()).filePath(QStringLiteral("Documents"));
    QVERIFY(QDir().mkpath(targetRoot));

    QSet<QString> indexedNames;
    QDirIterator it(fixtureRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString sourcePath = it.next();
        const QString relPath = QDir(fixtureRoot).relativeFilePath(sourcePath);
        const QString targetPath = QDir(targetRoot).filePath(relPath);

        const QFileInfo targetInfo(targetPath);
        QVERIFY(QDir().mkpath(targetInfo.path()));

        const QFileInfo sourceInfo(sourcePath);
        const QString extension = sourceInfo.suffix().toLower();
        const QString name = sourceInfo.fileName();
        const QString content = syntheticContentForFile(sourcePath);
        const auto now = static_cast<double>(QDateTime::currentSecsSinceEpoch());

        auto itemId = store.upsertItem(
            targetPath,
            name,
            extension.isEmpty() ? QString() : QStringLiteral(".") + extension,
            classifyKind(extension),
            std::max<int64_t>(1, static_cast<int64_t>(sourceInfo.size())),
            now,
            now,
            QString(),
            QStringLiteral("normal"),
            QFileInfo(targetPath).path());
        QVERIFY2(itemId.has_value(), qPrintable(QStringLiteral("Failed to upsert item: %1").arg(name)));

        bs::Chunk chunk;
        chunk.chunkId = bs::computeChunkId(targetPath, 0);
        chunk.filePath = targetPath;
        chunk.chunkIndex = 0;
        chunk.content = content;
        chunk.byteOffset = 0;
        const std::vector<bs::Chunk> chunks = {chunk};
        QVERIFY2(store.insertChunks(itemId.value(), name, targetPath, chunks),
                 qPrintable(QStringLiteral("Failed to insert chunks for %1").arg(name)));
        indexedNames.insert(name.toLower());
    }

    // Deterministic fixture preflight: all expected files must exist.
    QStringList invalidFixtureCases;
    for (const QueryCase& c : cases) {
        if (!indexedNames.contains(c.expectedFileName.toLower())) {
            invalidFixtureCases.append(
                QStringLiteral("[%1] missing expected fixture file \"%2\"")
                    .arg(c.id, c.expectedFileName));
        }
    }
    if (!invalidFixtureCases.isEmpty()) {
        QFAIL(qPrintable(QStringLiteral("invalid_fixture_case:\n%1")
                             .arg(invalidFixtureCases.join(QStringLiteral("\n")))));
    }

    const QString queryBinary = findQueryBinary();
    QVERIFY2(!queryBinary.isEmpty(), "Could not locate betterspotlight-query binary");

    const QString querySocket = bs::ServiceBase::socketPath(QStringLiteral("query"));
    QFile::remove(querySocket);

    QProcess queryProcess;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), tempHome.path());
    env.insert(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR"), dataDir);
    queryProcess.setProcessEnvironment(env);
    queryProcess.setProgram(queryBinary);
    queryProcess.setArguments({});
    queryProcess.setProcessChannelMode(QProcess::ForwardedChannels);
    queryProcess.start();
    QVERIFY2(queryProcess.waitForStarted(5000), "Failed to start query service process");
    auto processGuard = qScopeGuard([&]() {
        if (queryProcess.state() != QProcess::NotRunning) {
            queryProcess.kill();
            queryProcess.waitForFinished(2000);
        }
    });

    bs::SocketClient queryClient;
    QVERIFY2(waitForQueryConnection(queryClient, querySocket, 5000),
             qPrintable(QStringLiteral("Failed to connect to query service socket: %1").arg(querySocket)));

    const bool hasVectorRequiredCases = std::any_of(
        cases.begin(), cases.end(), [](const QueryCase& c) {
            return c.requiresVectors;
        });
    bool vectorsReady = !hasVectorRequiredCases;
    QString vectorUnavailableReason;
    if (hasVectorRequiredCases) {
        QJsonObject rebuildParams;
        QJsonArray includePaths;
        includePaths.append(targetRoot);
        rebuildParams[QStringLiteral("includePaths")] = includePaths;
        const QJsonObject rebuildResponse = sendOrFail(
            queryClient, QStringLiteral("rebuildVectorIndex"), rebuildParams);
        if (rebuildResponse.value(QStringLiteral("type")).toString() != QStringLiteral("response")) {
            vectorUnavailableReason = QStringLiteral("rebuild_request_failed");
        } else {
            QElapsedTimer rebuildTimer;
            rebuildTimer.start();
            while (rebuildTimer.elapsed() < 120000) {
                const QJsonObject healthResponse = sendOrFail(queryClient, QStringLiteral("getHealth"));
                if (healthResponse.value(QStringLiteral("type")).toString() != QStringLiteral("response")) {
                    QTest::qWait(150);
                    continue;
                }
                const QJsonObject indexHealth = healthResponse.value(QStringLiteral("result"))
                                                    .toObject()
                                                    .value(QStringLiteral("indexHealth"))
                                                    .toObject();
                const QString status = indexHealth.value(
                    QStringLiteral("vectorRebuildStatus")).toString();
                if (status == QLatin1String("succeeded")) {
                    vectorsReady = true;
                    vectorUnavailableReason.clear();
                    break;
                }
                if (status == QLatin1String("failed")) {
                    vectorUnavailableReason = indexHealth.value(
                        QStringLiteral("vectorRebuildLastError")).toString(
                            QStringLiteral("vector_rebuild_failed"));
                    break;
                }
                QTest::qWait(150);
            }
            if (!vectorsReady && vectorUnavailableReason.isEmpty()) {
                vectorUnavailableReason = QStringLiteral("vector_rebuild_timeout");
            }
        }
    }

    int passed = 0;
    int skipped = 0;
    int semanticUnavailable = 0;
    QStringList failures;
    QJsonArray rankingMissDetails;
    QJsonArray semanticUnavailableDetails;

    for (const QueryCase& c : cases) {
        if (c.category == QLatin1String("typo_strict")) {
            ++skipped;
            continue;
        }

        if (c.requiresVectors && !vectorsReady) {
            ++semanticUnavailable;
            const QString detail = QStringLiteral(
                                       "[%1|%2] q=\"%3\" expect=\"%4\" semantic_unavailable (%5)")
                                       .arg(c.id,
                                            c.category,
                                            c.query,
                                            c.expectedFileName,
                                            vectorUnavailableReason);
            failures.append(detail);
            QJsonObject entry;
            entry[QStringLiteral("id")] = c.id;
            entry[QStringLiteral("category")] = c.category;
            entry[QStringLiteral("failureType")] = QStringLiteral("semantic_unavailable");
            entry[QStringLiteral("query")] = c.query;
            entry[QStringLiteral("expectedFileName")] = c.expectedFileName;
            entry[QStringLiteral("reason")] = vectorUnavailableReason;
            semanticUnavailableDetails.append(entry);
            continue;
        }

        QJsonObject params;
        params[QStringLiteral("query")] = c.query;
        params[QStringLiteral("limit")] = std::max(3, c.topN);
        params[QStringLiteral("queryMode")] = c.mode;
        params[QStringLiteral("debug")] = true;

        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonArray ranked = result.value(QStringLiteral("results")).toArray();
        QStringList inspected;
        const bool ok = containsExpectedFileInTopN(ranked, c.expectedFileName, c.topN, &inspected);
        if (ok) {
            ++passed;
            continue;
        }

        failures.append(QStringLiteral("[%1|%2] q=\"%3\" expect=\"%4\" topN=%5 saw=[%6]")
                            .arg(c.id,
                                 c.category,
                                 c.query,
                                 c.expectedFileName,
                                 QString::number(c.topN),
                                 inspected.join(QStringLiteral(", "))));
        QJsonObject entry;
        entry[QStringLiteral("id")] = c.id;
        entry[QStringLiteral("category")] = c.category;
        entry[QStringLiteral("failureType")] = QStringLiteral("ranking_miss");
        entry[QStringLiteral("query")] = c.query;
        entry[QStringLiteral("expectedFileName")] = c.expectedFileName;
        entry[QStringLiteral("inspectedTopN")] = inspected.join(QStringLiteral(", "));
        rankingMissDetails.append(entry);
    }

    const int total = static_cast<int>(cases.size()) - skipped - semanticUnavailable;
    QVERIFY2(total > 0, "No evaluated baseline cases after skips");
    const double passRate = (100.0 * static_cast<double>(passed)) / static_cast<double>(total);
    const double gatePassRate = root.value(QStringLiteral("gatePassRate")).toDouble(90.0);
    const int requiredPasses = static_cast<int>(std::ceil((gatePassRate / 100.0) * total));

    const QString reportPath = qEnvironmentVariable("BS_RELEVANCE_FIXTURE_REPORT_PATH").trimmed();
    if (!reportPath.isEmpty()) {
        QJsonObject report;
        report[QStringLiteral("baselinesPath")] = baselinesPath;
        report[QStringLiteral("fixtureRoot")] = fixtureRoot;
        report[QStringLiteral("dbPath")] = dbPath;
        report[QStringLiteral("gatePassRate")] = gatePassRate;
        report[QStringLiteral("totalCases")] = total;
        report[QStringLiteral("passedCases")] = passed;
        report[QStringLiteral("passRate")] = passRate;
        report[QStringLiteral("requiredPasses")] = requiredPasses;
        report[QStringLiteral("skippedCases")] = skipped;
        report[QStringLiteral("semanticUnavailableCount")] = semanticUnavailable;
        report[QStringLiteral("rankingMisses")] = rankingMissDetails;
        report[QStringLiteral("semanticUnavailableCases")] = semanticUnavailableDetails;
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

    queryClient.sendRequest(QStringLiteral("shutdown"), {}, 1000);
    queryProcess.waitForFinished(5000);
    if (queryProcess.state() != QProcess::NotRunning) {
        queryProcess.kill();
        queryProcess.waitForFinished(2000);
    }
    processGuard.dismiss();

    if (!failures.isEmpty()) {
        for (const QString& line : failures) {
            qInfo().noquote() << line;
        }
    }
    QVERIFY2(passRate >= gatePassRate,
             qPrintable(QStringLiteral("Fixture relevance gate failed: %1/%2 (%3%%) below gate %4%% (required %5)")
                            .arg(passed)
                            .arg(total)
                            .arg(QString::number(passRate, 'f', 2))
                            .arg(QString::number(gatePassRate, 'f', 1))
                            .arg(requiredPasses)));
}

QTEST_MAIN(TestQueryServiceRelevanceFixture)
#include "test_query_service_relevance_fixture.moc"
