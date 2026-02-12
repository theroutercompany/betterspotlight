#include <QtTest/QtTest>

#include "core/index/sqlite_store.h"
#include "core/ipc/socket_client.h"
#include "core/ipc/socket_server.h"
#include "core/shared/chunk.h"
#include "core/shared/ipc_messages.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"
#include "ipc_test_utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <limits>
#include <optional>
#include <vector>

namespace {

std::optional<int64_t> seedItem(bs::SQLiteStore& store,
                                const QString& path,
                                const QString& content,
                                int64_t size,
                                double modifiedAtSecs)
{
    const QFileInfo info(path);
    const QString extension = info.suffix();
    const bs::ItemKind kind = (extension.compare(QStringLiteral("md"), Qt::CaseInsensitive) == 0)
        ? bs::ItemKind::Markdown
        : bs::ItemKind::Text;
    auto itemId = store.upsertItem(path,
                                   info.fileName(),
                                   extension,
                                   kind,
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

bool addVectorMapping(bs::VectorStore& vectorStore,
                      bs::VectorIndex& index,
                      int64_t itemId,
                      const std::vector<float>& embedding,
                      const std::string& generationId,
                      const std::string& modelId,
                      int dimensions)
{
    const uint64_t label = index.addVector(embedding.data());
    if (label == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    return vectorStore.addMapping(itemId,
                                  label,
                                  modelId,
                                  generationId,
                                  dimensions,
                                  "cpu",
                                  0,
                                  "active");
}

std::vector<float> makeSparseEmbedding(int dims, int hotIndex, float hotValue, int warmIndex = -1, float warmValue = 0.0f)
{
    std::vector<float> embedding(static_cast<size_t>(dims), 0.0f);
    if (hotIndex >= 0 && hotIndex < dims) {
        embedding[static_cast<size_t>(hotIndex)] = hotValue;
    }
    if (warmIndex >= 0 && warmIndex < dims) {
        embedding[static_cast<size_t>(warmIndex)] = warmValue;
    }
    return embedding;
}

QJsonObject makeInferenceOk(uint64_t id, const QJsonObject& body)
{
    QJsonObject result = body;
    result[QStringLiteral("status")] = QStringLiteral("ok");
    result[QStringLiteral("elapsedMs")] = 1;
    return bs::IpcMessage::makeResponse(id, result);
}

} // namespace

class TestQueryServiceSemanticOffload : public QObject {
    Q_OBJECT

private slots:
    void testSemanticAndRerankOffloadBranches();
};

void TestQueryServiceSemanticOffload::testSemanticAndRerankOffloadBranches()
{
    QTemporaryDir tempHome;
    QTemporaryDir fixtureRoot;
    QTemporaryDir socketRoot(QStringLiteral("/tmp/bsqsoffload-XXXXXX"));
    QVERIFY(tempHome.isValid());
    QVERIFY(fixtureRoot.isValid());
    QVERIFY(socketRoot.isValid());

    const QString dataDir =
        QDir(tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    QVERIFY(QDir().mkpath(dataDir));

    const QString docsDir = QDir(fixtureRoot.path()).filePath(QStringLiteral("Docs"));
    const QString outsideDir = QDir(fixtureRoot.path()).filePath(QStringLiteral("Outside"));
    QVERIFY(QDir().mkpath(docsDir));
    QVERIFY(QDir().mkpath(outsideDir));

    const QString dbPath = QDir(dataDir).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const auto idA = seedItem(
        store,
        QDir(docsDir).filePath(QStringLiteral("semantic-alpha.md")),
        QStringLiteral("orchid nebula context one"),
        /*size=*/350,
        /*modifiedAtSecs=*/500.0);
    const auto idB = seedItem(
        store,
        QDir(docsDir).filePath(QStringLiteral("semantic-beta.md")),
        QStringLiteral("orchid nebula context two"),
        /*size=*/360,
        /*modifiedAtSecs=*/500.0);
    const auto idOutside = seedItem(
        store,
        QDir(outsideDir).filePath(QStringLiteral("semantic-outside.md")),
        QStringLiteral("orchid nebula outside"),
        /*size=*/360,
        /*modifiedAtSecs=*/500.0);
    const auto idWrongType = seedItem(
        store,
        QDir(docsDir).filePath(QStringLiteral("semantic-gamma.txt")),
        QStringLiteral("orchid nebula wrong type"),
        /*size=*/360,
        /*modifiedAtSecs=*/500.0);
    const auto idTiny = seedItem(
        store,
        QDir(docsDir).filePath(QStringLiteral("semantic-tiny.md")),
        QStringLiteral("orchid nebula tiny"),
        /*size=*/4,
        /*modifiedAtSecs=*/500.0);

    QVERIFY(idA.has_value());
    QVERIFY(idB.has_value());
    QVERIFY(idOutside.has_value());
    QVERIFY(idWrongType.has_value());
    QVERIFY(idTiny.has_value());

    constexpr int kDims = 384;
    const std::string generationId = "v1";
    const std::string modelId = "fake-semantic-model";

    bs::VectorIndex::IndexMetadata meta;
    meta.dimensions = kDims;
    meta.modelId = modelId;
    meta.generationId = generationId;
    meta.provider = "cpu";

    bs::VectorIndex index(meta);
    QVERIFY(index.create());

    bs::VectorStore vectorStore(store.rawDb());
    QVERIFY(addVectorMapping(vectorStore, index, idA.value(), makeSparseEmbedding(kDims, 0, 1.0f),
                             generationId, modelId, kDims));
    QVERIFY(addVectorMapping(vectorStore, index, idB.value(), makeSparseEmbedding(kDims, 0, 0.98f, 1, 0.02f),
                             generationId, modelId, kDims));
    QVERIFY(addVectorMapping(vectorStore, index, idOutside.value(), makeSparseEmbedding(kDims, 0, 0.99f, 1, 0.01f),
                             generationId, modelId, kDims));
    QVERIFY(addVectorMapping(vectorStore, index, idWrongType.value(), makeSparseEmbedding(kDims, 0, 0.97f, 1, 0.03f),
                             generationId, modelId, kDims));
    QVERIFY(addVectorMapping(vectorStore, index, idTiny.value(), makeSparseEmbedding(kDims, 0, 0.96f, 1, 0.04f),
                             generationId, modelId, kDims));

    bs::VectorStore::GenerationState activeState;
    activeState.generationId = generationId;
    activeState.modelId = modelId;
    activeState.dimensions = kDims;
    activeState.provider = "cpu";
    activeState.state = "active";
    activeState.progressPct = 100.0;
    activeState.active = true;
    QVERIFY(vectorStore.upsertGenerationState(activeState));

    const QString indexPath = QDir(dataDir).filePath(QStringLiteral("vectors-v1.hnsw"));
    const QString metaPath = QDir(dataDir).filePath(QStringLiteral("vectors-v1.meta"));
    QVERIFY(index.save(indexPath.toStdString(), metaPath.toStdString()));

    QVERIFY(store.setSetting(QStringLiteral("activeVectorGeneration"), QStringLiteral("v1")));
    QVERIFY(store.setSetting(QStringLiteral("embeddingEnabled"), QStringLiteral("1")));
    QVERIFY(store.setSetting(QStringLiteral("inferenceServiceEnabled"), QStringLiteral("1")));
    QVERIFY(store.setSetting(QStringLiteral("inferenceEmbedOffloadEnabled"), QStringLiteral("1")));
    QVERIFY(store.setSetting(QStringLiteral("inferenceRerankOffloadEnabled"), QStringLiteral("1")));
    QVERIFY(store.setSetting(QStringLiteral("inferenceShadowModeEnabled"), QStringLiteral("0")));
    QVERIFY(store.setSetting(QStringLiteral("queryRouterEnabled"), QStringLiteral("0")));
    QVERIFY(store.setSetting(QStringLiteral("fastEmbeddingEnabled"), QStringLiteral("0")));
    QVERIFY(store.setSetting(QStringLiteral("dualEmbeddingFusionEnabled"), QStringLiteral("0")));
    QVERIFY(store.setSetting(QStringLiteral("semanticThresholdNaturalLanguageBase"), QStringLiteral("0.20")));
    QVERIFY(store.setSetting(QStringLiteral("semanticThresholdMin"), QStringLiteral("0.10")));
    QVERIFY(store.setSetting(QStringLiteral("semanticThresholdMax"), QStringLiteral("0.90")));
    QVERIFY(store.setSetting(QStringLiteral("semanticOnlySafetySimilarityWeakNatural"), QStringLiteral("0.20")));
    QVERIFY(store.setSetting(QStringLiteral("semanticOnlySafetySimilarityDefault"), QStringLiteral("0.20")));
    QVERIFY(store.setSetting(QStringLiteral("relaxedSemanticOnlyMinWeakNatural"), QStringLiteral("0.20")));
    QVERIFY(store.setSetting(QStringLiteral("relaxedSemanticOnlyMinDefault"), QStringLiteral("0.20")));
    QVERIFY(store.setSetting(QStringLiteral("strictLexicalWeakCutoff"), QStringLiteral("999")));
    QVERIFY(store.setSetting(QStringLiteral("rerankerCascadeEnabled"), QStringLiteral("1")));
    QVERIFY(store.setSetting(QStringLiteral("rerankerStage1Max"), QStringLiteral("10")));
    QVERIFY(store.setSetting(QStringLiteral("rerankerStage2Max"), QStringLiteral("10")));
    QVERIFY(store.setSetting(QStringLiteral("rerankBudgetMs"), QStringLiteral("400")));

    const QString queryBinary = bs::test::resolveServiceBinary(QStringLiteral("betterspotlight-query"));
    QVERIFY2(!queryBinary.isEmpty(), "Could not resolve betterspotlight-query binary");

    const QString socketDir = socketRoot.path();
    const QString querySocketPath = QDir(socketDir).filePath(QStringLiteral("query.sock"));
    const QString inferenceSocketPath = QDir(socketDir).filePath(QStringLiteral("inference.sock"));
    QFile::remove(querySocketPath);
    QFile::remove(inferenceSocketPath);

    QProcess queryProcess;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), tempHome.path());
    env.insert(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR"), dataDir);
    env.insert(QStringLiteral("BETTERSPOTLIGHT_SOCKET_DIR"), socketDir);
    queryProcess.setProcessEnvironment(env);
    queryProcess.setProgram(queryBinary);
    queryProcess.setArguments({});
    queryProcess.setProcessChannelMode(QProcess::ForwardedChannels);
    queryProcess.start();
    QVERIFY2(queryProcess.waitForStarted(5000), "Failed to start query process");
    auto processGuard = qScopeGuard([&]() {
        if (queryProcess.state() != QProcess::NotRunning) {
            queryProcess.kill();
            queryProcess.waitForFinished(2000);
        }
    });

    bs::SocketClient queryClient;
    QVERIFY2(bs::test::waitForSocketConnection(queryClient, querySocketPath, 5000),
             "Failed to connect query client to socket");

    bs::SocketServer fakeInference;
    fakeInference.setRequestHandler([&](const QJsonObject& request) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();

        if (method == QLatin1String("embed_query")) {
            QJsonObject payload;
            payload[QStringLiteral("modelRole")] = QStringLiteral("bi-encoder");
            QJsonObject result;
            QJsonArray embedding;
            embedding.append(1.0);
            for (int i = 1; i < kDims; ++i) {
                embedding.append(0.0);
            }
            result[QStringLiteral("embedding")] = embedding;
            payload[QStringLiteral("result")] = result;
            return makeInferenceOk(id, payload);
        }

        if (method == QLatin1String("rerank_fast") || method == QLatin1String("rerank_strong")) {
            const QJsonArray candidates = params.value(QStringLiteral("candidates")).toArray();
            QJsonArray scores;
            for (const QJsonValue& value : candidates) {
                const QJsonObject candidate = value.toObject();
                const qint64 itemId = candidate.value(QStringLiteral("itemId")).toInteger();
                const QString path = candidate.value(QStringLiteral("path")).toString();

                double score = 0.10;
                if (path.endsWith(QStringLiteral("semantic-alpha.md"))) {
                    score = (method == QLatin1String("rerank_strong")) ? 0.95 : 0.55;
                } else if (path.endsWith(QStringLiteral("semantic-beta.md"))) {
                    score = (method == QLatin1String("rerank_strong")) ? 0.15 : 0.54;
                } else {
                    score = (method == QLatin1String("rerank_strong")) ? 0.05 : 0.53;
                }

                QJsonObject scoreObj;
                scoreObj[QStringLiteral("itemId")] = itemId;
                scoreObj[QStringLiteral("score")] = score;
                scores.append(scoreObj);
            }
            QJsonObject payload;
            payload[QStringLiteral("modelRole")] =
                (method == QLatin1String("rerank_fast"))
                    ? QStringLiteral("cross-encoder-fast")
                    : QStringLiteral("cross-encoder");
            QJsonObject result;
            result[QStringLiteral("scores")] = scores;
            payload[QStringLiteral("result")] = result;
            return makeInferenceOk(id, payload);
        }

        if (method == QLatin1String("get_inference_health")) {
            QJsonObject payload;
            payload[QStringLiteral("connected")] = true;
            QJsonObject roleStatus;
            roleStatus[QStringLiteral("bi-encoder")] = QStringLiteral("ready");
            roleStatus[QStringLiteral("cross-encoder-fast")] = QStringLiteral("ready");
            roleStatus[QStringLiteral("cross-encoder")] = QStringLiteral("ready");
            payload[QStringLiteral("roleStatusByModel")] = roleStatus;
            payload[QStringLiteral("queueDepthByRole")] = QJsonObject{};
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
    QVERIFY2(fakeInference.listen(inferenceSocketPath), "Failed to listen fake inference socket");

    QJsonObject filters;
    QJsonArray includePaths;
    includePaths.append(docsDir);
    filters[QStringLiteral("includePaths")] = includePaths;
    QJsonArray fileTypes;
    fileTypes.append(QStringLiteral("md"));
    filters[QStringLiteral("fileTypes")] = fileTypes;
    filters[QStringLiteral("modifiedAfter")] = 100.0;
    filters[QStringLiteral("modifiedBefore")] = 1000.0;
    filters[QStringLiteral("minSize")] = 100;
    filters[QStringLiteral("maxSize")] = 1000;

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("latent intent retrieval");
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("filters")] = filters;
        const QJsonObject response = bs::test::sendRequestOrEmpty(
            queryClient, QStringLiteral("search"), params, 7000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject result = bs::test::resultPayload(response);
        const QJsonArray ranked = result.value(QStringLiteral("results")).toArray();
        QVERIFY(ranked.size() >= 2);

        const QJsonObject debugInfo = result.value(QStringLiteral("debugInfo")).toObject();
        QVERIFY(debugInfo.value(QStringLiteral("semanticCandidates")).toInt() >= 2);
        QVERIFY(debugInfo.value(QStringLiteral("strongSemanticCandidates")).toInt() >= 2);
        QVERIFY(debugInfo.value(QStringLiteral("rerankDepthApplied")).toInt() >= 2);
        const QJsonObject rerankerStages = debugInfo.value(QStringLiteral("rerankerStagesApplied")).toObject();
        QVERIFY(rerankerStages.value(QStringLiteral("stage1Depth")).toInt() >= 2);
        QVERIFY(rerankerStages.value(QStringLiteral("stage2Depth")).toInt() >= 2);
        QVERIFY(rerankerStages.value(QStringLiteral("stage1Applied")).toBool(false));
        QVERIFY(rerankerStages.value(QStringLiteral("stage2Applied")).toBool(false));

        for (const QJsonValue& value : ranked) {
            const QString path = value.toObject().value(QStringLiteral("path")).toString();
            QVERIFY(path.startsWith(docsDir));
            QVERIFY(path.endsWith(QStringLiteral(".md")));
        }
        QCOMPARE(ranked.first().toObject().value(QStringLiteral("path")).toString(),
                 QDir(docsDir).filePath(QStringLiteral("semantic-alpha.md")));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("latent intent retrieval");
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("strict");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("filters")] = filters;
        const QJsonObject response = bs::test::sendRequestOrEmpty(
            queryClient, QStringLiteral("search"), params, 7000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject debugInfo = bs::test::resultPayload(response)
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("queryMode")).toString(),
                 QStringLiteral("strict"));
        QCOMPARE(debugInfo.value(QStringLiteral("rewriteReason")).toString(),
                 QStringLiteral("strict_mode"));
    }

    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("orchd nebla retrieval");
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("queryMode")] = QStringLiteral("relaxed");
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("filters")] = filters;
        const QJsonObject response = bs::test::sendRequestOrEmpty(
            queryClient, QStringLiteral("search"), params, 7000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject debugInfo = bs::test::resultPayload(response)
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QCOMPARE(debugInfo.value(QStringLiteral("queryMode")).toString(),
                 QStringLiteral("relaxed"));
        QVERIFY(!debugInfo.value(QStringLiteral("rewriteReason")).toString().isEmpty());
    }

    QVERIFY(store.setSetting(QStringLiteral("rerankerCascadeEnabled"), QStringLiteral("0")));
    {
        QJsonObject params;
        params[QStringLiteral("query")] = QStringLiteral("latent intent retrieval");
        params[QStringLiteral("debug")] = true;
        params[QStringLiteral("limit")] = 10;
        params[QStringLiteral("filters")] = filters;
        const QJsonObject response = bs::test::sendRequestOrEmpty(
            queryClient, QStringLiteral("search"), params, 7000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject debugInfo = bs::test::resultPayload(response)
                                          .value(QStringLiteral("debugInfo"))
                                          .toObject();
        QVERIFY(!debugInfo.value(QStringLiteral("rerankerCascadeEnabled")).toBool(true));
        QVERIFY(debugInfo.value(QStringLiteral("rerankDepthApplied")).toInt() >= 1);
    }

    {
        const QJsonObject response = bs::test::sendRequestOrEmpty(
            queryClient, QStringLiteral("getHealth"), {}, 5000);
        QVERIFY(bs::test::isResponse(response));
        const QJsonObject indexHealth = bs::test::resultPayload(response)
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        QVERIFY(indexHealth.value(QStringLiteral("inferenceServiceConnected")).toBool(false));
        const QJsonObject roleStatus = indexHealth
                                           .value(QStringLiteral("inferenceRoleStatusByModel"))
                                           .toObject();
        QCOMPARE(roleStatus.value(QStringLiteral("bi-encoder")).toString(),
                 QStringLiteral("ready"));
    }

    queryClient.sendRequest(QStringLiteral("shutdown"), {}, 1000);
    queryProcess.waitForFinished(5000);
    processGuard.dismiss();
    fakeInference.close();
}

QTEST_MAIN(TestQueryServiceSemanticOffload)
#include "test_query_service_semantic_offload.moc"
