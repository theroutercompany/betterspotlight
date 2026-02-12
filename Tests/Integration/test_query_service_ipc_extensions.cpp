#include <QtTest/QtTest>

#include "core/index/sqlite_store.h"
#include "core/ipc/socket_server.h"
#include "core/shared/chunk.h"
#include "core/shared/ipc_messages.h"
#include "ipc_test_utils.h"
#include "service_process_harness.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <optional>
#include <vector>

#include <unistd.h>

namespace {

std::optional<int64_t> seedItem(bs::SQLiteStore& store,
                                const QString& path,
                                const QString& content,
                                int64_t size,
                                double modifiedAtSecs)
{
    const QFileInfo info(path);
    auto itemId = store.upsertItem(path,
                                   info.fileName(),
                                   info.suffix(),
                                   bs::ItemKind::Text,
                                   size,
                                   modifiedAtSecs - 10.0,
                                   modifiedAtSecs,
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

} // namespace

class TestQueryServiceIpcExtensions : public QObject {
    Q_OBJECT

private slots:
    void testExtendedIpcBranches();
};

void TestQueryServiceIpcExtensions::testExtendedIpcBranches()
{
    QTemporaryDir tempHome;
    QTemporaryDir docsRoot;
    QVERIFY(tempHome.isValid());
    QVERIFY(docsRoot.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString docsDir = QDir(docsRoot.path()).filePath(QStringLiteral("Docs"));
    const QString excludedDir = QDir(docsDir).filePath(QStringLiteral("excluded"));
    QVERIFY(QDir().mkpath(docsDir));
    QVERIFY(QDir().mkpath(excludedDir));

    const QString seededPath = QDir(docsDir).filePath(QStringLiteral("coverage-report.md"));
    const QString excludedPath = QDir(excludedDir).filePath(QStringLiteral("skip-report.md"));
    const QString oldPath = QDir(docsDir).filePath(QStringLiteral("old-report.md"));
    const QString tinyPath = QDir(docsDir).filePath(QStringLiteral("tiny-report.md"));
    const QString txtPath = QDir(docsDir).filePath(QStringLiteral("coverage-report.txt"));
    const QString emptyPath = QDir(docsDir).filePath(QStringLiteral("empty-report.md"));

    const auto seededId = seedItem(
        store,
        seededPath,
        QStringLiteral(
            "This coverage report documents branch coverage marker behavior across indexing "
            "retries and quarterly downloads summary for release planning with additional "
            "diagnostics and remediation notes for ranking quality."),
        /*size=*/320,
        /*modifiedAtSecs=*/300.0);
    const auto excludedId = seedItem(
        store,
        excludedPath,
        QStringLiteral("branch coverage marker excluded"),
        /*size=*/320,
        /*modifiedAtSecs=*/300.0);
    const auto oldId = seedItem(
        store,
        oldPath,
        QStringLiteral("branch coverage marker old"),
        /*size=*/320,
        /*modifiedAtSecs=*/40.0);
    const auto tinyId = seedItem(
        store,
        tinyPath,
        QStringLiteral("branch coverage marker tiny"),
        /*size=*/4,
        /*modifiedAtSecs=*/300.0);
    const auto txtId = seedItem(
        store,
        txtPath,
        QStringLiteral("branch coverage marker txt"),
        /*size=*/320,
        /*modifiedAtSecs=*/300.0);

    QVERIFY(seededId.has_value());
    QVERIFY(excludedId.has_value());
    QVERIFY(oldId.has_value());
    QVERIFY(tinyId.has_value());
    QVERIFY(txtId.has_value());
    const double nowSecs = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const auto emptyItemId = store.upsertItem(emptyPath,
                                              QStringLiteral("empty-report.md"),
                                              QStringLiteral("md"),
                                              bs::ItemKind::Markdown,
                                              0,
                                              nowSecs - 10.0,
                                              nowSecs,
                                              QString(),
                                              QStringLiteral("normal"),
                                              docsDir);
    QVERIFY(emptyItemId.has_value());

    QVERIFY(store.setSetting(QStringLiteral("qaSnippetEnabled"), QStringLiteral("1")));
    QVERIFY(store.setSetting(QStringLiteral("inferenceServiceEnabled"), QStringLiteral("1")));

    const QString bsignorePath = QDir(tempHome.path()).filePath(QStringLiteral(".bsignore"));
    {
        QFile bsignoreFile(bsignorePath);
        QVERIFY(bsignoreFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bsignoreFile.write("*/excluded/*\n");
        bsignoreFile.close();
    }

    QVERIFY(store.recordFailure(
        seededId.value(),
        QStringLiteral("extraction"),
        QStringLiteral("Critical parser crash in extension coverage test")));
    QVERIFY(store.recordFailure(
        excludedId.value(),
        QStringLiteral("extraction"),
        QStringLiteral("File does not exist or is not a regular file")));

    const QString pidRoot = QStringLiteral("/tmp/betterspotlight-%1")
                                .arg(static_cast<qulonglong>(::getuid()));
    QVERIFY(QDir().mkpath(pidRoot));
    const auto writePidFile = [&](const QString& serviceName) {
        QFile pidFile(QDir(pidRoot).filePath(serviceName + QStringLiteral(".pid")));
        QVERIFY(pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        pidFile.write(QByteArray::number(QCoreApplication::applicationPid()));
        pidFile.close();
    };
    writePidFile(QStringLiteral("query"));
    writePidFile(QStringLiteral("indexer"));
    writePidFile(QStringLiteral("extractor"));
    writePidFile(QStringLiteral("inference"));

    bs::test::ServiceProcessHarness harness(
        QStringLiteral("query"), QStringLiteral("betterspotlight-query"));
    bs::test::ServiceLaunchConfig launch;
    launch.homeDir = tempHome.path();
    launch.dataDir = dataDir;
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    QVERIFY2(harness.start(launch), "Failed to start query service");

    const QString socketDir = QFileInfo(harness.socketPath()).absolutePath();
    const QString indexerSocketPath = QDir(socketDir).filePath(QStringLiteral("indexer.sock"));
    const QString inferenceSocketPath = QDir(socketDir).filePath(QStringLiteral("inference.sock"));
    QFile::remove(indexerSocketPath);
    QFile::remove(inferenceSocketPath);

    QJsonArray queueRoots;
    int queuePending = 3000;
    bs::SocketServer fakeIndexer;
    fakeIndexer.setRequestHandler([&](const QJsonObject& request) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        if (method == QLatin1String("getQueueStatus")) {
            QJsonObject result;
            result[QStringLiteral("pending")] = queuePending;
            result[QStringLiteral("processing")] = 1;
            result[QStringLiteral("failed")] = 0;
            result[QStringLiteral("dropped")] = 0;
            result[QStringLiteral("paused")] = false;
            result[QStringLiteral("preparing")] = 1;
            result[QStringLiteral("writing")] = 0;
            result[QStringLiteral("coalesced")] = 2;
            result[QStringLiteral("staleDropped")] = 0;
            result[QStringLiteral("prepWorkers")] = 1;
            result[QStringLiteral("writerBatchDepth")] = 1;
            result[QStringLiteral("roots")] = queueRoots;
            return bs::IpcMessage::makeResponse(id, result);
        }
        if (method == QLatin1String("ping")) {
            QJsonObject result;
            result[QStringLiteral("pong")] = true;
            return bs::IpcMessage::makeResponse(id, result);
        }
        return bs::IpcMessage::makeError(
            id, bs::IpcErrorCode::NotFound, QStringLiteral("unsupported"));
    });
    QVERIFY2(fakeIndexer.listen(indexerSocketPath), "Failed to start fake indexer socket");

    bs::SocketServer fakeInference;
    fakeInference.setRequestHandler([&](const QJsonObject& request) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        if (method == QLatin1String("get_inference_health")) {
            QJsonObject payload;
            payload[QStringLiteral("connected")] = true;
            QJsonObject roleStatus;
            roleStatus[QStringLiteral("bi-encoder")] = QStringLiteral("ready");
            roleStatus[QStringLiteral("cross-encoder")] = QStringLiteral("degraded");
            payload[QStringLiteral("roleStatusByModel")] = roleStatus;
            QJsonObject queueDepth;
            queueDepth[QStringLiteral("bi-encoder")] = 1;
            payload[QStringLiteral("queueDepthByRole")] = queueDepth;
            payload[QStringLiteral("timeoutCountByRole")] = QJsonObject{};
            payload[QStringLiteral("failureCountByRole")] = QJsonObject{};
            payload[QStringLiteral("restartCountByRole")] = QJsonObject{};
            return bs::IpcMessage::makeResponse(id, payload);
        }
        if (method == QLatin1String("ping")) {
            QJsonObject payload;
            payload[QStringLiteral("pong")] = true;
            return bs::IpcMessage::makeResponse(id, payload);
        }
        return bs::IpcMessage::makeError(
            id, bs::IpcErrorCode::NotFound, QStringLiteral("unsupported"));
    });
    QVERIFY2(fakeInference.listen(inferenceSocketPath), "Failed to start fake inference socket");

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("quarterly downloads summary");
        params[QStringLiteral("debug")] = true;
        const QJsonObject response = harness.request(QStringLiteral("search"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject debugInfo = bs::test::resultPayload(response)
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("plannerReason")).toString(),
                 QStringLiteral("query_location_hint"));
    }

    {
        QJsonObject filters;
        QJsonArray fileTypes;
        fileTypes.append(QStringLiteral("md"));
        filters[QStringLiteral("fileTypes")] = fileTypes;
        QJsonArray includePaths;
        includePaths.append(docsDir);
        filters[QStringLiteral("includePaths")] = includePaths;
        QJsonArray excludePaths;
        excludePaths.append(excludedDir);
        filters[QStringLiteral("excludePaths")] = excludePaths;
        filters[QStringLiteral("modifiedAfter")] = 100.0;
        filters[QStringLiteral("modifiedBefore")] = 1000.0;
        filters[QStringLiteral("minSize")] = 100;
        filters[QStringLiteral("maxSize")] = 1000;

        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("branch coverage marker");
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("filters")] = filters;
        const QJsonObject response = harness.request(QStringLiteral("search"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        const QJsonObject filtersDebug = debugInfo.value(QStringLiteral("filters")).toObject();
        QVERIFY(filtersDebug.value(QStringLiteral("hasFilters")).toBool(false));
        QVERIFY(filtersDebug.contains(QStringLiteral("modifiedAfter")));
        QVERIFY(filtersDebug.contains(QStringLiteral("modifiedBefore")));
        QVERIFY(filtersDebug.contains(QStringLiteral("minSize")));
        QVERIFY(filtersDebug.contains(QStringLiteral("maxSize")));

        const QJsonArray results = result.value(QStringLiteral("results")).toArray();
        QVERIFY(!results.isEmpty());
        for (const QJsonValue& value : results) {
            const QString path = value.toObject().value(QStringLiteral("path")).toString();
            QVERIFY(path.startsWith(docsDir));
            QVERIFY(!path.startsWith(excludedDir));
            QVERIFY(path.endsWith(QStringLiteral(".md")));
        }
    }

    {
        queueRoots = QJsonArray();
        for (int i = 0; i < 40; ++i) {
            queueRoots.append(QStringLiteral("/roots/r%1").arg(i));
        }
        const QJsonObject response = harness.request(QStringLiteral("getHealth"), {}, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject indexHealth = bs::test::resultPayload(response)
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        const QJsonObject advisory = indexHealth.value(QStringLiteral("retrievalAdvisory")).toObject();
        QCOMPARE(advisory.value(QStringLiteral("code")).toString(),
                 QStringLiteral("root_fanout_recommended"));
        QVERIFY(indexHealth.value(QStringLiteral("inferenceServiceConnected")).toBool(false));
        const QJsonObject roleStatus =
            indexHealth.value(QStringLiteral("inferenceRoleStatusByModel")).toObject();
        QCOMPARE(roleStatus.value(QStringLiteral("bi-encoder")).toString(), QStringLiteral("ready"));
        QVERIFY(indexHealth.value(QStringLiteral("recentErrors")).toArray().size() >= 1);
        const QJsonObject memoryByService =
            indexHealth.value(QStringLiteral("memoryByService")).toObject();
        QVERIFY(memoryByService.value(QStringLiteral("query")).toObject()
                    .value(QStringLiteral("available")).toBool(false));
    }

    {
        queueRoots = QJsonArray{tempHome.path()};
        queuePending = 3500;
        const QJsonObject response = harness.request(QStringLiteral("getHealth"), {}, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject indexHealth = bs::test::resultPayload(response)
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        const QJsonObject advisory = indexHealth.value(QStringLiteral("retrievalAdvisory")).toObject();
        QCOMPARE(advisory.value(QStringLiteral("code")).toString(),
                 QStringLiteral("curated_roots_recommended"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("limit")] = -10;
        params[QStringLiteral("offset")] = -3;
        const QJsonObject response = harness.request(QStringLiteral("getHealthDetails"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject details = bs::test::resultPayload(response)
                                        .value(QStringLiteral("details"))
                                        .toObject();
        QCOMPARE(details.value(QStringLiteral("failuresLimit")).toInt(), 1);
        QCOMPARE(details.value(QStringLiteral("failuresOffset")).toInt(), 0);
        const QJsonArray failures = details.value(QStringLiteral("failures")).toArray();
        QCOMPARE(failures.size(), 1);
        const int criticalRows = details.value(QStringLiteral("criticalFailureRows")).toInt();
        const int expectedGapRows = details.value(QStringLiteral("expectedGapFailureRows")).toInt();
        QCOMPARE(criticalRows + expectedGapRows, failures.size());
        const QString severity = failures.first().toObject().value(QStringLiteral("severity")).toString();
        QVERIFY(severity == QStringLiteral("critical") || severity == QStringLiteral("expected_gap"));
        const QJsonObject processStats = details.value(QStringLiteral("processStats")).toObject();
        QVERIFY(processStats.value(QStringLiteral("query")).toObject()
                    .value(QStringLiteral("available")).toBool(false));
        const QJsonObject bsignore = details.value(QStringLiteral("bsignore")).toObject();
        QVERIFY(bsignore.value(QStringLiteral("fileExists")).toBool(false));
        QVERIFY(bsignore.value(QStringLiteral("patternCount")).toInt() >= 1);
    }

    QVERIFY(store.setSetting(QStringLiteral("qaSnippetEnabled"), QStringLiteral("0")));
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("branch coverage");
        params[QStringLiteral("path")] = seededPath;
        const QJsonObject response = harness.request(QStringLiteral("getAnswerSnippet"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("feature_disabled"));
    }

    QVERIFY(store.setSetting(QStringLiteral("qaSnippetEnabled"), QStringLiteral("1")));
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("a an the");
        params[QStringLiteral("path")] = seededPath;
        const QJsonObject response = harness.request(QStringLiteral("getAnswerSnippet"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("query_too_short"));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("branch coverage");
        const QJsonObject response = harness.request(QStringLiteral("getAnswerSnippet"), params, 5000);
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("branch coverage");
        params[QStringLiteral("path")] = QStringLiteral("/no/such/path.md");
        const QJsonObject response = harness.request(QStringLiteral("getAnswerSnippet"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("available")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("item_not_found"));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("branch coverage");
        params[QStringLiteral("path")] = emptyPath;
        const QJsonObject response = harness.request(QStringLiteral("getAnswerSnippet"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("available")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("no_content"));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("zeta omega kappa unmatched tokens");
        params[QStringLiteral("path")] = seededPath;
        params[QStringLiteral("maxChunks")] = 1;
        const QJsonObject response =
            harness.request(QStringLiteral("get_answer_snippet"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(!result.value(QStringLiteral("available")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("no_answer"));
    }
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("quarterly downloads summary");
        params[QStringLiteral("itemId")] = static_cast<qint64>(seededId.value());
        params[QStringLiteral("maxChars")] = 90;
        params[QStringLiteral("maxChunks")] = 8;
        const QJsonObject response = harness.request(QStringLiteral("getAnswerSnippet"), params, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("available")).toBool(false));
        QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("ok"));
        QVERIFY(!result.value(QStringLiteral("answer")).toString().trimmed().isEmpty());
        QVERIFY(result.value(QStringLiteral("confidence")).toDouble() > 0.0);
        QVERIFY(result.contains(QStringLiteral("source")));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("recordFeedback"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("itemId")] = static_cast<qint64>(seededId.value());
        params[QStringLiteral("action")] = QStringLiteral("opened");
        params[QStringLiteral("query")] = QStringLiteral("branch coverage marker");
        params[QStringLiteral("position")] = 1;
        const QJsonObject response = harness.request(QStringLiteral("recordFeedback"), params);
        QVERIFY(bs::test::isResponse(response));
        QVERIFY(bs::test::resultPayload(response).value(QStringLiteral("recorded")).toBool(false));
    }

    {
        const QJsonObject response = harness.request(QStringLiteral("getFrequency"));
        QVERIFY(bs::test::isError(response));
        QCOMPARE(bs::test::errorPayload(response).value(QStringLiteral("code")).toInt(),
                 static_cast<int>(bs::IpcErrorCode::InvalidParams));
    }
    {
        QJsonObject params;
        params[QStringLiteral("itemId")] = static_cast<qint64>(seededId.value());
        const QJsonObject response = harness.request(QStringLiteral("getFrequency"), params);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        QVERIFY(result.value(QStringLiteral("openCount")).toInt() >= 1);
        QVERIFY(result.value(QStringLiteral("frequencyTier")).toInt() >= 1);
    }

    fakeInference.close();
    fakeIndexer.close();
}

QTEST_MAIN(TestQueryServiceIpcExtensions)
#include "test_query_service_ipc_extensions.moc"
