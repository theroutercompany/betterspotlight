#include <QtTest/QtTest>

#define private public
#include "app/search_controller.h"
#undef private

#include "core/ipc/message.h"
#include "core/ipc/service_base.h"
#include "core/ipc/socket_server.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

namespace {

class EnvVarGuard {
public:
    explicit EnvVarGuard(const char* variableName)
        : name(variableName),
          hadValue(qEnvironmentVariableIsSet(variableName)),
          previousValue(qgetenv(variableName))
    {
    }

    ~EnvVarGuard()
    {
        if (hadValue) {
            qputenv(name.constData(), previousValue);
        } else {
            qunsetenv(name.constData());
        }
    }

private:
    QByteArray name;
    bool hadValue = false;
    QByteArray previousValue;
};

QJsonObject makeResult(qint64 itemId,
                       const QString& label,
                       const QString& kind,
                       int frequency)
{
    const QString slug = label.toLower().replace(QLatin1Char(' '), QLatin1Char('-'));

    QJsonObject metadata;
    metadata[QStringLiteral("fileSize")] = 1024 + static_cast<int>(itemId);
    metadata[QStringLiteral("modificationDate")] = QStringLiteral("2026-05-24T12:00:00Z");
    metadata[QStringLiteral("frequency")] = frequency;

    QJsonObject result;
    result[QStringLiteral("itemId")] = itemId;
    result[QStringLiteral("path")] = QStringLiteral("/tmp/%1").arg(slug);
    result[QStringLiteral("name")] = label;
    result[QStringLiteral("kind")] = kind;
    result[QStringLiteral("matchType")] = QStringLiteral("hybrid");
    result[QStringLiteral("score")] = 0.9;
    result[QStringLiteral("snippet")] = QStringLiteral("snippet for %1").arg(label);
    result[QStringLiteral("contentAvailable")] = true;
    result[QStringLiteral("availabilityStatus")] = QStringLiteral("available");
    result[QStringLiteral("metadata")] = metadata;
    return result;
}

QJsonObject makeSearchResponse(uint64_t id, const QString& label)
{
    QJsonArray results;
    results.append(makeResult(100 + id, QStringLiteral("%1 recent").arg(label),
                              QStringLiteral("file"), 4));
    results.append(makeResult(200 + id, QStringLiteral("%1 folder").arg(label),
                              QStringLiteral("directory"), 0));
    results.append(makeResult(300 + id, QStringLiteral("%1 file").arg(label),
                              QStringLiteral("file"), 0));

    QJsonObject result;
    result[QStringLiteral("results")] = results;
    return bs::IpcMessage::makeResponse(id, result);
}

void removeSocketIfPresent(const QString& socketPath)
{
    QFile::remove(socketPath);
}

} // namespace

class TestSearchController : public QObject {
    Q_OBJECT

private slots:
    void testSlowSearchResponseDoesNotBlockController();
    void testStaleAsyncSearchResponseDoesNotOverwriteNewerResults();
    void testClearResultsCancelsInFlightSearchAndLateResponse();
    void testSearchErrorClearsStaleResultsForCurrentQuery();
    void testMalformedSearchPayloadDoesNotCreateBlankSelectableRows();
    void testAnswerSnippetUpdatesAsynchronously();
    void testAnswerSnippetLateResponseIgnoredAfterClearResults();
    void testCorruptResultRowsAreNotSelectable();
};

void TestSearchController::testSlowSearchResponseDoesNotBlockController()
{
    EnvVarGuard socketDirGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.path().toUtf8());

    const QString querySocketPath = bs::ServiceBase::socketPath(QStringLiteral("query"));
    const QString indexerSocketPath = bs::ServiceBase::socketPath(QStringLiteral("indexer"));
    removeSocketIfPresent(querySocketPath);
    removeSocketIfPresent(indexerSocketPath);

    QVector<QString> queryMethods;
    bs::SocketServer queryServer;
    queryServer.setRequestHandler(
        [&queryServer, &queryMethods](const QJsonObject& request,
                                      bs::SocketServer::RequestResponder responder) {
            const QString method = request.value(QStringLiteral("method")).toString();
            queryMethods.append(method);
            if (request.value(QStringLiteral("type")).toString() == QLatin1String("notification")) {
                return;
            }

            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (method == QLatin1String("search")) {
                QTimer::singleShot(120, &queryServer, [responder, id]() {
                    responder.send(makeSearchResponse(id, QStringLiteral("project")));
                });
                return;
            }

            responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
        });
    QVERIFY(queryServer.listen(querySocketPath));

    QVector<bool> activeSignals;
    bs::SocketServer indexerServer;
    indexerServer.setRequestHandler(
        [&activeSignals](const QJsonObject& request,
                         bs::SocketServer::RequestResponder responder) {
            const QString method = request.value(QStringLiteral("method")).toString();
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (method == QLatin1String("setUserActive")) {
                activeSignals.append(request.value(QStringLiteral("params"))
                                         .toObject()
                                         .value(QStringLiteral("active"))
                                         .toBool());
            }
            responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
        });
    QVERIFY(indexerServer.listen(indexerSocketPath));

    bs::SearchController controller;
    QSignalSpy searchingSpy(&controller, &bs::SearchController::isSearchingChanged);
    QSignalSpy rowsSpy(&controller, &bs::SearchController::resultRowsChanged);

    controller.m_query = QStringLiteral("project");
    QElapsedTimer elapsed;
    elapsed.start();
    controller.executeSearch();

    QVERIFY2(elapsed.elapsed() < 500,
             qPrintable(QStringLiteral("executeSearch blocked for %1ms").arg(elapsed.elapsed())));
    QVERIFY(controller.isSearching());
    QVERIFY(searchingSpy.count() >= 1);

    QTRY_COMPARE_WITH_TIMEOUT(controller.results().size(), 3, 2000);
    QVERIFY(!controller.isSearching());
    QVERIFY(rowsSpy.count() >= 1);
    QVERIFY(queryMethods.contains(QStringLiteral("search")));
    QVERIFY(queryMethods.contains(QStringLiteral("record_behavior_event")));
    QTRY_VERIFY_WITH_TIMEOUT(activeSignals.contains(true), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(activeSignals.contains(false), 1000);

    const QVariantList rows = controller.resultRows();
    QCOMPARE(rows.size(), 6);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("rowType")).toString(),
             QStringLiteral("header"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Recently Opened"));
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("rowType")).toString(),
             QStringLiteral("result"));
    QCOMPARE(rows.at(2).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Folders"));
    QCOMPARE(rows.at(4).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Files"));
    QCOMPARE(controller.selectedIndex(), 1);

    controller.moveSelection(1);
    QCOMPARE(controller.selectedIndex(), 3);
    controller.moveSelection(1);
    QCOMPARE(controller.selectedIndex(), 5);
}

void TestSearchController::testStaleAsyncSearchResponseDoesNotOverwriteNewerResults()
{
    EnvVarGuard socketDirGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.path().toUtf8());

    const QString querySocketPath = bs::ServiceBase::socketPath(QStringLiteral("query"));
    const QString indexerSocketPath = bs::ServiceBase::socketPath(QStringLiteral("indexer"));
    removeSocketIfPresent(querySocketPath);
    removeSocketIfPresent(indexerSocketPath);

    bs::SocketServer queryServer;
    queryServer.setRequestHandler(
        [&queryServer](const QJsonObject& request,
                       bs::SocketServer::RequestResponder responder) {
            if (request.value(QStringLiteral("type")).toString() == QLatin1String("notification")) {
                return;
            }

            const QString method = request.value(QStringLiteral("method")).toString();
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (method != QLatin1String("search")) {
                responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
                return;
            }

            const QString query = request.value(QStringLiteral("params"))
                                      .toObject()
                                      .value(QStringLiteral("query"))
                                      .toString();
            const int delayMs = query == QLatin1String("first") ? 160 : 20;
            QTimer::singleShot(delayMs, &queryServer, [responder, id, query]() {
                responder.send(makeSearchResponse(id, query));
            });
        });
    QVERIFY(queryServer.listen(querySocketPath));

    bs::SocketServer indexerServer;
    indexerServer.setRequestHandler(
        [](const QJsonObject& request, bs::SocketServer::RequestResponder responder) {
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
        });
    QVERIFY(indexerServer.listen(indexerSocketPath));

    bs::SearchController controller;
    controller.m_query = QStringLiteral("first");
    controller.executeSearch();
    controller.m_query = QStringLiteral("second");
    controller.executeSearch();

    QTRY_COMPARE_WITH_TIMEOUT(controller.results().size(), 3, 2000);
    QCOMPARE(controller.results().first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("second recent"));
    QVERIFY(!controller.isSearching());

    QTest::qWait(250);
    QCOMPARE(controller.results().first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("second recent"));
}

void TestSearchController::testClearResultsCancelsInFlightSearchAndLateResponse()
{
    EnvVarGuard socketDirGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.path().toUtf8());

    const QString querySocketPath = bs::ServiceBase::socketPath(QStringLiteral("query"));
    const QString indexerSocketPath = bs::ServiceBase::socketPath(QStringLiteral("indexer"));
    removeSocketIfPresent(querySocketPath);
    removeSocketIfPresent(indexerSocketPath);

    bs::SocketServer queryServer;
    queryServer.setRequestHandler(
        [&queryServer](const QJsonObject& request,
                       bs::SocketServer::RequestResponder responder) {
            if (request.value(QStringLiteral("type")).toString() == QLatin1String("notification")) {
                return;
            }

            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (request.value(QStringLiteral("method")).toString() == QLatin1String("search")) {
                QTimer::singleShot(140, &queryServer, [responder, id]() {
                    responder.send(makeSearchResponse(id, QStringLiteral("late")));
                });
                return;
            }
            responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
        });
    QVERIFY(queryServer.listen(querySocketPath));

    QVector<bool> activeSignals;
    bs::SocketServer indexerServer;
    indexerServer.setRequestHandler(
        [&activeSignals](const QJsonObject& request,
                         bs::SocketServer::RequestResponder responder) {
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (request.value(QStringLiteral("method")).toString()
                == QLatin1String("setUserActive")) {
                activeSignals.append(request.value(QStringLiteral("params"))
                                         .toObject()
                                         .value(QStringLiteral("active"))
                                         .toBool());
            }
            responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
        });
    QVERIFY(indexerServer.listen(indexerSocketPath));

    bs::SearchController controller;
    QSignalSpy searchingSpy(&controller, &bs::SearchController::isSearchingChanged);
    controller.m_query = QStringLiteral("clear me");
    controller.executeSearch();

    QVERIFY(controller.isSearching());
    controller.clearResults();

    QVERIFY(!controller.isSearching());
    QCOMPARE(controller.query(), QString());
    QVERIFY(controller.results().isEmpty());
    QVERIFY(controller.resultRows().isEmpty());
    QCOMPARE(controller.selectedIndex(), -1);
    QVERIFY(searchingSpy.count() >= 2);
    QTRY_VERIFY_WITH_TIMEOUT(activeSignals.contains(true), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(activeSignals.contains(false), 1000);

    QTest::qWait(240);
    QVERIFY(controller.results().isEmpty());
    QVERIFY(controller.resultRows().isEmpty());
    QVERIFY(!controller.isSearching());
}

void TestSearchController::testSearchErrorClearsStaleResultsForCurrentQuery()
{
    EnvVarGuard socketDirGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.path().toUtf8());

    const QString querySocketPath = bs::ServiceBase::socketPath(QStringLiteral("query"));
    const QString indexerSocketPath = bs::ServiceBase::socketPath(QStringLiteral("indexer"));
    removeSocketIfPresent(querySocketPath);
    removeSocketIfPresent(indexerSocketPath);

    bs::SocketServer queryServer;
    queryServer.setRequestHandler(
        [](const QJsonObject& request, bs::SocketServer::RequestResponder responder) {
            if (request.value(QStringLiteral("type")).toString() == QLatin1String("notification")) {
                return;
            }

            const QString method = request.value(QStringLiteral("method")).toString();
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (method != QLatin1String("search")) {
                responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
                return;
            }

            const QString query = request.value(QStringLiteral("params"))
                                      .toObject()
                                      .value(QStringLiteral("query"))
                                      .toString();
            if (query == QLatin1String("broken")) {
                responder.send(bs::IpcMessage::makeError(
                    id,
                    bs::IpcErrorCode::InternalError,
                    QStringLiteral("forced search failure")));
                return;
            }
            responder.send(makeSearchResponse(id, query));
        });
    QVERIFY(queryServer.listen(querySocketPath));

    bs::SocketServer indexerServer;
    indexerServer.setRequestHandler(
        [](const QJsonObject& request, bs::SocketServer::RequestResponder responder) {
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
        });
    QVERIFY(indexerServer.listen(indexerSocketPath));

    bs::SearchController controller;
    QSignalSpy rowsSpy(&controller, &bs::SearchController::resultRowsChanged);

    controller.m_query = QStringLiteral("working");
    controller.executeSearch();
    QTRY_COMPARE_WITH_TIMEOUT(controller.results().size(), 3, 2000);
    QVERIFY(!controller.resultRows().isEmpty());

    controller.m_query = QStringLiteral("broken");
    controller.executeSearch();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isSearching(), 2000);

    QVERIFY(controller.results().isEmpty());
    QVERIFY(controller.resultRows().isEmpty());
    QCOMPARE(controller.selectedIndex(), -1);
    QVERIFY(rowsSpy.count() >= 2);
}

void TestSearchController::testMalformedSearchPayloadDoesNotCreateBlankSelectableRows()
{
    bs::SearchController controller;

    QJsonArray results;
    results.append(QStringLiteral("not an object"));
    results.append(QJsonObject{});
    results.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("missing path")},
        {QStringLiteral("kind"), QStringLiteral("file")},
    });
    results.append(QJsonObject{
        {QStringLiteral("path"), QStringLiteral("   ")},
        {QStringLiteral("name"), QStringLiteral("blank path")},
    });
    results.append(QJsonObject{
        {QStringLiteral("itemId"), -42},
        {QStringLiteral("path"), QStringLiteral("/tmp/no-name.txt")},
        {QStringLiteral("kind"), QString()},
        {QStringLiteral("score"), -7.0},
        {QStringLiteral("availabilityStatus"), QStringLiteral("unexpected backend string")},
        {QStringLiteral("metadata"), QJsonObject{
             {QStringLiteral("fileSize"), -1},
             {QStringLiteral("frequency"), -3},
         }},
    });
    results.append(QJsonObject{
        {QStringLiteral("itemId"), 88},
        {QStringLiteral("path"), QStringLiteral("/tmp/folder")},
        {QStringLiteral("name"), QStringLiteral("Folder")},
        {QStringLiteral("kind"), QStringLiteral("directory")},
        {QStringLiteral("availabilityStatus"), QStringLiteral("offline_placeholder")},
        {QStringLiteral("metadata"), QJsonObject{
             {QStringLiteral("fileSize"), 4096},
             {QStringLiteral("frequency"), 2},
         }},
    });

    controller.parseSearchResponse(
        bs::IpcMessage::makeResponse(99, QJsonObject{{QStringLiteral("results"), results}}));

    QCOMPARE(controller.results().size(), 2);
    const QVariantMap fallbackName = controller.results().at(0).toMap();
    QCOMPARE(fallbackName.value(QStringLiteral("itemId")).toLongLong(), 0);
    QCOMPARE(fallbackName.value(QStringLiteral("path")).toString(),
             QStringLiteral("/tmp/no-name.txt"));
    QCOMPARE(fallbackName.value(QStringLiteral("name")).toString(),
             QStringLiteral("no-name.txt"));
    QCOMPARE(fallbackName.value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
    QCOMPARE(fallbackName.value(QStringLiteral("score")).toDouble(), 0.0);
    QCOMPARE(fallbackName.value(QStringLiteral("fileSize")).toLongLong(), 0);
    QCOMPARE(fallbackName.value(QStringLiteral("frequency")).toLongLong(), 0);
    QCOMPARE(fallbackName.value(QStringLiteral("availabilityStatus")).toString(),
             QStringLiteral("available"));

    const QVariantMap folder = controller.results().at(1).toMap();
    QCOMPARE(folder.value(QStringLiteral("itemId")).toLongLong(), 88);
    QCOMPARE(folder.value(QStringLiteral("availabilityStatus")).toString(),
             QStringLiteral("offline_placeholder"));

    const QVariantList rows = controller.resultRows();
    QCOMPARE(rows.size(), 4);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("rowType")).toString(),
             QStringLiteral("header"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Recently Opened"));
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("resultIndex")).toInt(), 1);
    QCOMPARE(rows.at(2).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Files"));
    QCOMPARE(rows.at(3).toMap().value(QStringLiteral("resultIndex")).toInt(), 0);
    QCOMPARE(controller.selectedIndex(), 1);
}

void TestSearchController::testAnswerSnippetUpdatesAsynchronously()
{
    EnvVarGuard socketDirGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.path().toUtf8());

    const QString querySocketPath = bs::ServiceBase::socketPath(QStringLiteral("query"));
    removeSocketIfPresent(querySocketPath);

    QJsonObject capturedParams;
    bs::SocketServer queryServer;
    queryServer.setRequestHandler(
        [&queryServer, &capturedParams](const QJsonObject& request,
                                        bs::SocketServer::RequestResponder responder) {
            if (request.value(QStringLiteral("type")).toString() == QLatin1String("notification")) {
                return;
            }

            const QString method = request.value(QStringLiteral("method")).toString();
            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (method != QLatin1String("getAnswerSnippet")) {
                responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
                return;
            }

            capturedParams = request.value(QStringLiteral("params")).toObject();
            QTimer::singleShot(120, &queryServer, [responder, id]() {
                responder.send(bs::IpcMessage::makeResponse(
                    id,
                    QJsonObject{
                        {QStringLiteral("available"), true},
                        {QStringLiteral("answer"), QStringLiteral("The webhook is verified here.")},
                        {QStringLiteral("reason"), QStringLiteral("ok")},
                        {QStringLiteral("confidence"), 0.82},
                        {QStringLiteral("source"), QStringLiteral("extractive_qa")},
                    }));
            });
        });
    QVERIFY(queryServer.listen(querySocketPath));

    bs::SearchController controller;
    controller.m_query = QStringLiteral("where is webhook verification?");
    QJsonArray results;
    results.append(makeResult(42, QStringLiteral("Webhook handler"), QStringLiteral("file"), 0));
    controller.parseSearchResponse(
        bs::IpcMessage::makeResponse(1, QJsonObject{{QStringLiteral("results"), results}}));
    QCOMPARE(controller.selectedIndex(), 1);

    QSignalSpy rowsSpy(&controller, &bs::SearchController::resultRowsChanged);
    QElapsedTimer elapsed;
    elapsed.start();
    const QVariantMap summary = controller.requestAnswerSnippet(controller.selectedIndex());

    QVERIFY2(elapsed.elapsed() < 200,
             qPrintable(QStringLiteral("requestAnswerSnippet blocked for %1ms")
                            .arg(elapsed.elapsed())));
    QVERIFY(!summary.value(QStringLiteral("ok")).toBool());
    QVERIFY(summary.value(QStringLiteral("pending")).toBool());
    QCOMPARE(summary.value(QStringLiteral("reason")).toString(), QStringLiteral("pending"));
    QCOMPARE(controller.results().at(0).toMap().value(QStringLiteral("answerStatus")).toString(),
             QStringLiteral("loading"));

    QTRY_COMPARE_WITH_TIMEOUT(
        controller.results().at(0).toMap().value(QStringLiteral("answerStatus")).toString(),
        QStringLiteral("ready"),
        2000);
    const QVariantMap item = controller.results().at(0).toMap();
    QCOMPARE(item.value(QStringLiteral("answerSnippet")).toString(),
             QStringLiteral("The webhook is verified here."));
    QCOMPARE(item.value(QStringLiteral("answerReason")).toString(), QStringLiteral("ok"));
    QCOMPARE(item.value(QStringLiteral("answerSource")).toString(),
             QStringLiteral("extractive_qa"));
    QVERIFY(item.value(QStringLiteral("answerConfidence")).toDouble() > 0.8);
    QVERIFY(!item.contains(QStringLiteral("answerRequestId")));
    QVERIFY(rowsSpy.count() >= 2);

    QCOMPARE(capturedParams.value(QStringLiteral("query")).toString(),
             QStringLiteral("where is webhook verification?"));
    QCOMPARE(capturedParams.value(QStringLiteral("itemId")).toInteger(), 42);
    QCOMPARE(capturedParams.value(QStringLiteral("path")).toString(),
             QStringLiteral("/tmp/webhook-handler"));
}

void TestSearchController::testAnswerSnippetLateResponseIgnoredAfterClearResults()
{
    EnvVarGuard socketDirGuard("BETTERSPOTLIGHT_SOCKET_DIR");
    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());
    qputenv("BETTERSPOTLIGHT_SOCKET_DIR", socketDir.path().toUtf8());

    const QString querySocketPath = bs::ServiceBase::socketPath(QStringLiteral("query"));
    removeSocketIfPresent(querySocketPath);

    bs::SocketServer queryServer;
    queryServer.setRequestHandler(
        [&queryServer](const QJsonObject& request,
                       bs::SocketServer::RequestResponder responder) {
            if (request.value(QStringLiteral("type")).toString() == QLatin1String("notification")) {
                return;
            }

            const uint64_t id = static_cast<uint64_t>(
                request.value(QStringLiteral("id")).toInteger());
            if (request.value(QStringLiteral("method")).toString()
                != QLatin1String("getAnswerSnippet")) {
                responder.send(bs::IpcMessage::makeResponse(id, QJsonObject{}));
                return;
            }

            QTimer::singleShot(140, &queryServer, [responder, id]() {
                responder.send(bs::IpcMessage::makeResponse(
                    id,
                    QJsonObject{
                        {QStringLiteral("available"), true},
                        {QStringLiteral("answer"), QStringLiteral("late answer")},
                    }));
            });
        });
    QVERIFY(queryServer.listen(querySocketPath));

    bs::SearchController controller;
    controller.m_query = QStringLiteral("answer me");
    QJsonArray results;
    results.append(makeResult(77, QStringLiteral("Answer target"), QStringLiteral("file"), 0));
    controller.parseSearchResponse(
        bs::IpcMessage::makeResponse(2, QJsonObject{{QStringLiteral("results"), results}}));

    const QVariantMap summary = controller.requestAnswerSnippet(controller.selectedIndex());
    QVERIFY(summary.value(QStringLiteral("pending")).toBool());
    QCOMPARE(controller.results().at(0).toMap().value(QStringLiteral("answerStatus")).toString(),
             QStringLiteral("loading"));

    controller.clearResults();
    QTest::qWait(240);
    QVERIFY(controller.results().isEmpty());
    QVERIFY(controller.resultRows().isEmpty());
    QCOMPARE(controller.selectedIndex(), -1);
}

void TestSearchController::testCorruptResultRowsAreNotSelectable()
{
    bs::SearchController controller;
    QJsonArray results;
    results.append(makeResult(5, QStringLiteral("Safe result"), QStringLiteral("file"), 0));
    controller.parseSearchResponse(
        bs::IpcMessage::makeResponse(3, QJsonObject{{QStringLiteral("results"), results}}));

    QVariantMap header;
    header[QStringLiteral("rowType")] = QStringLiteral("header");
    header[QStringLiteral("title")] = QStringLiteral("Injected");
    QVariantMap outOfRangeResult;
    outOfRangeResult[QStringLiteral("rowType")] = QStringLiteral("result");
    outOfRangeResult[QStringLiteral("resultIndex")] = 99;
    QVariantMap validResult;
    validResult[QStringLiteral("rowType")] = QStringLiteral("result");
    validResult[QStringLiteral("resultIndex")] = 0;

    controller.m_resultRows = QVariantList{header, outOfRangeResult, header, validResult};

    controller.setSelectedIndex(1);
    QCOMPARE(controller.selectedIndex(), 3);
    controller.setSelectedIndex(2);
    QCOMPARE(controller.selectedIndex(), 3);
    controller.moveSelection(-1);
    QCOMPARE(controller.selectedIndex(), -1);

    const QVariantMap badAnswer = controller.requestAnswerSnippet(1);
    QVERIFY(!badAnswer.value(QStringLiteral("ok")).toBool());
    QCOMPARE(badAnswer.value(QStringLiteral("reason")).toString(),
             QStringLiteral("invalid_index"));
}

QTEST_MAIN(TestSearchController)
#include "test_search_controller.moc"
