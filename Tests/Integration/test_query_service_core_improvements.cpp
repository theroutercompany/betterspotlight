#include <QtTest/QtTest>

#include "core/ipc/message.h"
#include "core/ipc/service_base.h"
#include "core/ipc/socket_client.h"
#include "core/ipc/socket_server.h"
#include "core/index/sqlite_store.h"
#include "core/shared/chunk.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <optional>

namespace {

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

std::optional<int64_t> upsertItem(
    bs::SQLiteStore& store,
    const QString& path,
    const QString& extension,
    bs::ItemKind kind,
    const QString& content)
{
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    QFileInfo info(path);
    auto itemId = store.upsertItem(
        path,
        info.fileName(),
        extension,
        kind,
        static_cast<int64_t>(content.size() > 0 ? content.size() : 1),
        now,
        now,
        QString(),
        QStringLiteral("normal"),
        info.path());
    if (!itemId.has_value()) {
        return std::nullopt;
    }

    bs::Chunk chunk;
    chunk.chunkId = bs::computeChunkId(path, 0);
    chunk.filePath = path;
    chunk.chunkIndex = 0;
    chunk.content = content;
    chunk.byteOffset = 0;
    const std::vector<bs::Chunk> chunks = {chunk};
    if (!store.insertChunks(itemId.value(), info.fileName(), path, chunks)) {
        return std::nullopt;
    }
    return itemId;
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

} // namespace

class TestQueryServiceCoreImprovements : public QObject {
    Q_OBJECT

private slots:
    void testCoreBehaviorViaIpc();
};

void TestQueryServiceCoreImprovements::testCoreBehaviorViaIpc()
{
    QTemporaryDir tempHome;
    QVERIFY2(tempHome.isValid(), "Failed to create temporary HOME directory");

    const QString dataDir = QDir(tempHome.path())
                                .filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));

    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY2(storeOpt.has_value(), "Failed to initialize test SQLite store");
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString docsDir = QDir(tempHome.path()).filePath(QStringLiteral("Documents"));
    QVERIFY(QDir().mkpath(docsDir));

    // Parser/filter corpus.
    const QString pdfPath = QDir(docsDir).filePath(QStringLiteral("breaking-sound-barrier.pdf"));
    const QString txtPath = QDir(docsDir).filePath(QStringLiteral("breaking-sound-barrier.txt"));
    QVERIFY(upsertItem(store, pdfPath, QStringLiteral(".pdf"), bs::ItemKind::Pdf,
                       QStringLiteral("breaking sound barrier report")).has_value());
    QVERIFY(upsertItem(store, txtPath, QStringLiteral(".txt"), bs::ItemKind::Text,
                       QStringLiteral("breaking sound barrier report")).has_value());

    // Typo guardrail corpus.
    const QString bankingPath = QDir(docsDir).filePath(QStringLiteral("banking-report.txt"));
    QVERIFY(upsertItem(store, bankingPath, QStringLiteral(".txt"), bs::ItemKind::Text,
                       QStringLiteral("banking report report report report q1 summary")).has_value());
    const QString apiDeploymentPath = QDir(docsDir).filePath(QStringLiteral("API-deployment-guide.md"));
    QVERIFY(upsertItem(store, apiDeploymentPath, QStringLiteral(".md"), bs::ItemKind::Markdown,
                       QStringLiteral("API deployment guide for release operations")).has_value());
    const QString baselineClipboardPath = QDir(docsDir).filePath(QStringLiteral("meeting-rollout-notes.md"));
    QVERIFY(upsertItem(store, baselineClipboardPath, QStringLiteral(".md"), bs::ItemKind::Markdown,
                       QStringLiteral("rollout checklist reliability agenda")).has_value());
    const QString targetedClipboardPath = QDir(docsDir).filePath(QStringLiteral("deep-dive-notes.md"));
    QVERIFY(upsertItem(store, targetedClipboardPath, QStringLiteral(".md"), bs::ItemKind::Markdown,
                       QStringLiteral("rollout checklist reliability agenda")).has_value());

    // Placeholder/offline corpus.
    const QString creditPath = QDir(docsDir).filePath(QStringLiteral("credit report.pdf"));
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    auto creditId = store.upsertItem(
        creditPath,
        QStringLiteral("credit report.pdf"),
        QStringLiteral(".pdf"),
        bs::ItemKind::Pdf,
        1024,
        now,
        now,
        QString(),
        QStringLiteral("normal"),
        docsDir);
    QVERIFY(creditId.has_value());
    QVERIFY(store.recordFailure(
        creditId.value(),
        QStringLiteral("extraction"),
        QStringLiteral("File appears to be a cloud placeholder (size reported but no content readable)")));

    const QString queryBinary = findQueryBinary();
    QVERIFY2(!queryBinary.isEmpty(), "Could not locate betterspotlight-query binary");

    const QString querySocket = bs::ServiceBase::socketPath(QStringLiteral("query"));
    const QString indexerSocket = bs::ServiceBase::socketPath(QStringLiteral("indexer"));
    QFile::remove(querySocket);
    QFile::remove(indexerSocket);

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

    // Queue source should be unavailable when no indexer responds.
    {
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("getHealth"));
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject indexHealth = response.value(QStringLiteral("result"))
                                            .toObject()
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        QCOMPARE(indexHealth.value(QStringLiteral("queueSource")).toString(),
                 QStringLiteral("unavailable"));
        QCOMPARE(indexHealth.value(QStringLiteral("healthStatusReason")).toString(),
                 QStringLiteral("indexer_unavailable"));
        QCOMPARE(indexHealth.value(QStringLiteral("criticalFailures")).toInt(), 0);
        QCOMPARE(indexHealth.value(QStringLiteral("expectedGapFailures")).toInt(), 1);
    }

    // Start fake indexer and verify queue parity fields.
    bs::SocketServer fakeIndexer;
    fakeIndexer.setRequestHandler([&tempHome](const QJsonObject& request) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        if (method == QLatin1String("getQueueStatus")) {
            QJsonObject result;
            result[QStringLiteral("pending")] = 4200;
            result[QStringLiteral("processing")] = 2;
            result[QStringLiteral("failed")] = 0;
            result[QStringLiteral("dropped")] = 7;
            result[QStringLiteral("paused")] = false;
            result[QStringLiteral("preparing")] = 2;
            result[QStringLiteral("writing")] = 0;
            result[QStringLiteral("coalesced")] = 11;
            result[QStringLiteral("staleDropped")] = 3;
            result[QStringLiteral("prepWorkers")] = 3;
            result[QStringLiteral("writerBatchDepth")] = 1;
            QJsonArray roots;
            roots.append(tempHome.path());
            result[QStringLiteral("roots")] = roots;
            return bs::IpcMessage::makeResponse(id, result);
        }
        if (method == QLatin1String("ping")) {
            QJsonObject result;
            result[QStringLiteral("pong")] = true;
            return bs::IpcMessage::makeResponse(id, result);
        }
        return bs::IpcMessage::makeError(
            id, bs::IpcErrorCode::NotFound, QStringLiteral("Unsupported method"));
    });
    QVERIFY2(fakeIndexer.listen(indexerSocket), "Failed to start fake indexer socket server");

    {
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("getHealth"));
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject indexHealth = response.value(QStringLiteral("result"))
                                            .toObject()
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        QCOMPARE(indexHealth.value(QStringLiteral("queueSource")).toString(),
                 QStringLiteral("indexer_rpc"));
        QCOMPARE(indexHealth.value(QStringLiteral("healthStatusReason")).toString(),
                 QStringLiteral("healthy"));
        QCOMPARE(indexHealth.value(QStringLiteral("criticalFailures")).toInt(), 0);
        QCOMPARE(indexHealth.value(QStringLiteral("expectedGapFailures")).toInt(), 1);
        QCOMPARE(indexHealth.value(QStringLiteral("queuePending")).toInt(), 4200);
        QCOMPARE(indexHealth.value(QStringLiteral("queueInProgress")).toInt(), 2);
        QCOMPARE(indexHealth.value(QStringLiteral("queuePreparing")).toInt(), 2);
        QCOMPARE(indexHealth.value(QStringLiteral("queueCoalesced")).toInt(), 11);
        QVERIFY(indexHealth.value(QStringLiteral("retrievalAdvisory")).toObject().contains(
            QStringLiteral("code")));
    }

    // Health details endpoint should expose paginated failures + process/query stats.
    {
        QJsonObject params;
        params[QStringLiteral("limit")] = 25;
        params[QStringLiteral("offset")] = 0;
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("getHealthDetails"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonObject details = result.value(QStringLiteral("details")).toObject();
        QVERIFY(!details.isEmpty());

        const QJsonArray failures = details.value(QStringLiteral("failures")).toArray();
        QVERIFY(!failures.isEmpty());
        bool foundExpectedGap = false;
        for (const QJsonValue& value : failures) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("expectedGap")).toBool(false)) {
                foundExpectedGap = true;
                break;
            }
        }
        QVERIFY(foundExpectedGap);

        const QJsonObject processStats = details.value(QStringLiteral("processStats")).toObject();
        QVERIFY(processStats.contains(QStringLiteral("query")));
        const QJsonObject queryStats = processStats.value(QStringLiteral("query")).toObject();
        QVERIFY(queryStats.contains(QStringLiteral("available")));
        QVERIFY(queryStats.value(QStringLiteral("available")).isBool());

        QVERIFY(details.contains(QStringLiteral("queryStats")));
        QVERIFY(details.contains(QStringLiteral("bsignore")));
    }

    // Parser wiring + filter merge behavior.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("breaking sound barrier pdf");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("queryAfterParse")).toString(),
                 QStringLiteral("breaking sound barrier"));
        const QJsonArray parsedTypes = debugInfo.value(QStringLiteral("parsedTypes")).toArray();
        QVERIFY(!parsedTypes.isEmpty());
        QCOMPARE(parsedTypes.first().toString(), QStringLiteral("pdf"));
        QVERIFY(debugInfo.value(QStringLiteral("plannerApplied")).toBool(false));
        QCOMPARE(debugInfo.value(QStringLiteral("plannerReason")).toString(),
                 QStringLiteral("consumer_curated_prefilter"));
        QCOMPARE(debugInfo.value(QStringLiteral("queryClass")).toString(),
                 QStringLiteral("natural_language"));
        const double lexicalWeight =
            debugInfo.value(QStringLiteral("mergeLexicalWeightApplied")).toDouble();
        const double semanticWeight =
            debugInfo.value(QStringLiteral("mergeSemanticWeightApplied")).toDouble();
        const bool adaptiveApplied =
            debugInfo.value(QStringLiteral("adaptiveMergeWeightsApplied")).toBool(false);
        if (adaptiveApplied) {
            QVERIFY(std::abs(lexicalWeight - 0.45) < 1e-6);
            QVERIFY(std::abs(semanticWeight - 0.55) < 1e-6);
        } else {
            QVERIFY(std::abs(lexicalWeight - 0.55) < 1e-6);
            QVERIFY(std::abs(semanticWeight - 0.45) < 1e-6);
        }
        QVERIFY(std::abs((lexicalWeight + semanticWeight) - 1.0) < 1e-6);
        QVERIFY(debugInfo.contains(QStringLiteral("semanticOnlySuppressedCount")));
        QVERIFY(debugInfo.contains(QStringLiteral("semanticOnlyAdmittedCount")));
        QVERIFY(debugInfo.value(QStringLiteral("semanticOnlyAdmitReasonSummary")).isObject());
        const QJsonObject filtersDebug = debugInfo.value(QStringLiteral("filters")).toObject();
        const QJsonArray includePaths = filtersDebug.value(QStringLiteral("includePaths")).toArray();
        QVERIFY(!includePaths.isEmpty());

        const QJsonArray results = result.value(QStringLiteral("results")).toArray();
        QVERIFY(!results.isEmpty());
        for (const QJsonValue& value : results) {
            const QString name = value.toObject().value(QStringLiteral("name")).toString().toLower();
            QVERIFY2(name.endsWith(QStringLiteral(".pdf")),
                     qPrintable(QStringLiteral("Unexpected non-pdf result: %1").arg(name)));
        }
    }

    // Typo guardrail checks.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("banikng report");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("strict");
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject debugInfo = response.value(QStringLiteral("result"))
                                          .toObject()
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("queryMode")).toString(), QStringLiteral("strict"));
        QVERIFY(!debugInfo.value(QStringLiteral("rewriteApplied")).toBool(true));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("banikng");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("auto");
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject debugInfo = response.value(QStringLiteral("result"))
                                          .toObject()
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QVERIFY(debugInfo.contains(QStringLiteral("rewriteApplied")));
        QVERIFY(debugInfo.contains(QStringLiteral("rewriteReason")));
        QVERIFY(debugInfo.value(QStringLiteral("rewriteApplied")).toBool());
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("banikng repotr");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("auto");
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject debugInfo = response.value(QStringLiteral("result"))
                                          .toObject()
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QVERIFY(debugInfo.value(QStringLiteral("rewriteApplied")).toBool());
        const QJsonArray correctedTokens = debugInfo.value(QStringLiteral("correctedTokens")).toArray();
        QVERIFY2(correctedTokens.size() <= 2, "Auto-mode rewrite exceeded replacement budget");
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("API deplyoment guide");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("auto");
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(debugInfo.value(QStringLiteral("rewriteApplied")).toBool());
        const QJsonArray ranked = result.value(QStringLiteral("results")).toArray();
        QVERIFY(!ranked.isEmpty());
        const QString topName = ranked.first().toObject().value(QStringLiteral("name")).toString();
        QCOMPARE(topName, QStringLiteral("API-deployment-guide.md"));
    }

    // Clipboard context signals should boost matching path hints without storing raw clipboard text.
    {
        QJsonObject baseParams;
        baseParams[QStringLiteral("query")] = QStringLiteral("rollout checklist");
        baseParams[QStringLiteral("limit")] = 10;
        baseParams[QStringLiteral("debug")] = true;
        const QJsonObject baseResponse = sendOrFail(queryClient, QStringLiteral("search"), baseParams);
        QCOMPARE(baseResponse.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonArray baseResults = baseResponse.value(QStringLiteral("result"))
                                     .toObject()
                                     .value(QStringLiteral("results"))
                                     .toArray();
        QVERIFY(baseResults.size() >= 2);
        const QString baseTopPath = baseResults.first().toObject().value(QStringLiteral("path")).toString();
        QCOMPARE(baseTopPath, baselineClipboardPath);

        QJsonObject context;
        context[QStringLiteral("clipboardBasename")] = QStringLiteral("deep-dive-notes.md");
        context[QStringLiteral("clipboardDirname")] = QStringLiteral("documents");
        context[QStringLiteral("clipboardExtension")] = QStringLiteral("md");
        baseParams[QStringLiteral("context")] = context;
        const QJsonObject boostedResponse = sendOrFail(queryClient, QStringLiteral("search"), baseParams);
        QCOMPARE(boostedResponse.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonObject boostedResult = boostedResponse.value(QStringLiteral("result")).toObject();
        const QJsonArray boostedResults = boostedResult.value(QStringLiteral("results")).toArray();
        QVERIFY(boostedResults.size() >= 2);
        const QString boostedTopPath = boostedResults.first().toObject().value(QStringLiteral("path")).toString();
        QCOMPARE(boostedTopPath, targetedClipboardPath);

        const QJsonObject debugInfo = boostedResult.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(debugInfo.value(QStringLiteral("clipboardSignalsProvided")).toBool(false));
        QVERIFY(debugInfo.value(QStringLiteral("clipboardSignalBoostedResults")).toInt() > 0);
    }

    // Availability metadata for offline placeholder result.
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("credit report");
        params[QStringLiteral("limit")] = 10;
        const QJsonObject response = sendOrFail(queryClient, QStringLiteral("search"), params);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("response"));
        const QJsonArray results = response.value(QStringLiteral("result"))
                                       .toObject()
                                       .value(QStringLiteral("results"))
                                       .toArray();
        bool foundOffline = false;
        for (const QJsonValue& value : results) {
            const QJsonObject obj = value.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString().toLower();
            if (name.contains(QStringLiteral("credit report"))) {
                QCOMPARE(obj.value(QStringLiteral("availabilityStatus")).toString(),
                         QStringLiteral("offline_placeholder"));
                QVERIFY(!obj.value(QStringLiteral("contentAvailable")).toBool(true));
                foundOffline = true;
                break;
            }
        }
        QVERIFY(foundOffline);
    }

    fakeIndexer.close();

    // Graceful shutdown.
    queryClient.sendRequest(QStringLiteral("shutdown"), {}, 1000);
    queryProcess.waitForFinished(5000);
    if (queryProcess.state() != QProcess::NotRunning) {
        queryProcess.kill();
        queryProcess.waitForFinished(2000);
    }
    processGuard.dismiss();
}

QTEST_MAIN(TestQueryServiceCoreImprovements)
#include "test_query_service_core_improvements.moc"
