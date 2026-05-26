#include <QtTest/QtTest>

#define private public
#include "app/onboarding_controller.h"
#include "app/service_manager.h"
#undef private
#include "app/control_plane/control_plane_actor.h"
#include "core/ipc/message.h"
#include "core/models/model_manifest.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <chrono>
#include <thread>

namespace {

QString settingsPath()
{
    const QString overrideDir = qEnvironmentVariable("BETTERSPOTLIGHT_SETTINGS_DIR").trimmed();
    const QString settingsDir = overrideDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : overrideDir;
    return settingsDir + QStringLiteral("/settings.json");
}

void resetSettings()
{
    QFile::remove(settingsPath());
}

bool writeSettings(const QJsonObject& settings)
{
    const QString path = settingsPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(settings).toJson(QJsonDocument::Compact)) > 0;
}

QJsonObject readSettings()
{
    QFile file(settingsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

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

class FakeControlPlaneActor : public bs::ControlPlaneActor {
    Q_OBJECT

public:
    QJsonObject response;
    QString lastServiceName;
    QString lastMethod;
    QJsonObject lastParams;
    int lastTimeoutMs = 0;

    Q_INVOKABLE QJsonObject sendServiceRequestSync(const QString& serviceName,
                                                   const QString& method,
                                                   const QJsonObject& params = {},
                                                   int timeoutMs = 10000) override
    {
        lastServiceName = serviceName;
        lastMethod = method;
        lastParams = params;
        lastTimeoutMs = timeoutMs;
        return response;
    }
};

QJsonObject controlPlaneResponseForResult(const QJsonObject& result)
{
    QJsonObject out;
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("response")] = bs::IpcMessage::makeResponse(1, result);
    return out;
}

void attachFakeControlPlane(bs::ServiceManager& manager,
                            FakeControlPlaneActor* fake,
                            QThread& thread)
{
    manager.stopHealthThread();
    manager.stopControlPlaneThread();
    fake->moveToThread(&thread);
    thread.start();
    manager.m_controlPlaneActor = fake;
}

void detachFakeControlPlane(bs::ServiceManager& manager,
                            FakeControlPlaneActor* fake,
                            QThread& thread)
{
    manager.m_controlPlaneActor = nullptr;
    QMetaObject::invokeMethod(fake, "deleteLater", Qt::QueuedConnection);
    thread.quit();
    thread.wait();
}

} // namespace

class TestAppLifecycleStates : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void testTrayStateTransitions();
    void testInitialIndexingTriggerIsGatedAndSingleShot();
    void testOperationalReadinessIsHealthDriven();
    void testOperationalReadinessRejectsBrokenHealthSnapshots();
    void testStartWithNoServiceBinariesDoesNotCreatePhantomSession();
    void testReindexPathRejectsInvalidPathBeforeBackendRequest();
    void testServiceActionsRejectMissingPositiveAcknowledgement();
    void testServiceActionsAcceptPositiveAcknowledgementAndUpdateState();
    void testServiceActionsReportBackendFailure();
    void testServiceRequestSyncHandlesSameThreadControlPlane();
    void testRebuildAndReindexRequestsAcceptPositiveAcknowledgement();
    void testModelArtifactsRequireReadableFiles();
    void testModelDownloadRejectsDuplicateStartAndPublishesFailure();
    void testStopCancelsModelDownloadThreadBeforeJoining();
    void testSingleHomeRootWithoutHomeDirectoryMapFallsBackToCuratedRoots();
    void testHomeDirectoryMapDrivesIndexAndEmbeddingRoots();
    void testSkippedIndexRootsDoNotFallBackToCuratedRoots();
    void testIndexRootModeNormalizationMatchesSettingsContract();
    void testOnboardingAllSkippedClearsStaleIndexRoots();
    void testFdaCheckDoesNotTreatUngatedAutosaveAsGranted();
    void testOnboardingCompletionIsPersistedAndEmittedOnce();
};

void TestAppLifecycleStates::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
}

void TestAppLifecycleStates::cleanup()
{
    resetSettings();
}

void TestAppLifecycleStates::testTrayStateTransitions()
{
    bs::ServiceManager manager;
    QSignalSpy trayStateSpy(&manager, &bs::ServiceManager::trayStateChanged);

    QCOMPARE(manager.trayState(), QStringLiteral("indexing"));

    manager.m_allReady = true;
    manager.m_indexerStatus = QStringLiteral("running");
    manager.m_extractorStatus = QStringLiteral("running");
    manager.m_queryStatus = QStringLiteral("running");
    manager.m_indexingActive = false;
    manager.updateTrayState();

    QCOMPARE(manager.trayState(), QStringLiteral("idle"));
    QCOMPARE(trayStateSpy.count(), 1);

    manager.updateTrayState();
    QCOMPARE(trayStateSpy.count(), 1);

    manager.m_indexingActive = true;
    manager.updateTrayState();
    QCOMPARE(manager.trayState(), QStringLiteral("indexing"));
    QCOMPARE(trayStateSpy.count(), 2);

    manager.m_indexingActive = false;
    manager.m_queryStatus = QStringLiteral("crashed");
    manager.updateTrayState();
    QCOMPARE(manager.trayState(), QStringLiteral("error"));
    QCOMPARE(trayStateSpy.count(), 3);

    manager.m_queryStatus = QStringLiteral("running");
    manager.updateTrayState();
    QCOMPARE(manager.trayState(), QStringLiteral("idle"));
    QCOMPARE(trayStateSpy.count(), 4);
}

void TestAppLifecycleStates::testInitialIndexingTriggerIsGatedAndSingleShot()
{
    bs::ServiceManager manager;

    QVERIFY(!manager.m_initialIndexingStarted);

    manager.m_allReady = false;
    manager.triggerInitialIndexing();
    QVERIFY(!manager.m_initialIndexingStarted);

    manager.m_allReady = true;
    manager.triggerInitialIndexing();
    QVERIFY(manager.m_initialIndexingStarted);

    manager.triggerInitialIndexing();
    QVERIFY(manager.m_initialIndexingStarted);
}

void TestAppLifecycleStates::testOperationalReadinessIsHealthDriven()
{
    bs::ServiceManager manager;
    QSignalSpy readySpy(&manager, &bs::ServiceManager::allServicesReady);

    manager.m_allReady = false;
    manager.onAllServicesReady();
    QVERIFY(manager.m_operationalReadinessPending);
    QVERIFY(!manager.m_allReady);
    QCOMPARE(readySpy.count(), 0);

    QJsonObject snapshot;
    snapshot[QStringLiteral("overallStatus")] = QStringLiteral("rebuilding");
    snapshot[QStringLiteral("healthStatusReason")] = QStringLiteral("rebuilding");

    QJsonObject components;
    for (const QString& serviceName :
         {QStringLiteral("indexer"),
          QStringLiteral("query"),
          QStringLiteral("inference"),
          QStringLiteral("extractor")}) {
        components[serviceName] = QJsonObject{
            {QStringLiteral("state"), QStringLiteral("ready")},
        };
    }
    snapshot[QStringLiteral("components")] = components;

    QJsonObject inference;
    inference[QStringLiteral("connected")] = true;
    inference[QStringLiteral("roleStatusByModel")] = QJsonObject{
        {QStringLiteral("bi-encoder"), QStringLiteral("ready")},
        {QStringLiteral("cross-encoder"), QStringLiteral("ready")},
    };
    snapshot[QStringLiteral("inference")] = inference;

    snapshot[QStringLiteral("queue")] = QJsonObject{
        {QStringLiteral("rebuildStatus"), QStringLiteral("idle")},
    };

    QVERIFY(QMetaObject::invokeMethod(
        &manager,
        "onHealthSnapshotUpdated",
        Qt::DirectConnection,
        Q_ARG(QJsonObject, snapshot)));

    QVERIFY(!manager.m_operationalReadinessPending);
    QVERIFY(manager.m_allReady);
    QCOMPARE(readySpy.count(), 1);
}

void TestAppLifecycleStates::testOperationalReadinessRejectsBrokenHealthSnapshots()
{
    bs::ServiceManager manager;
    QString reason;

    QVERIFY(!manager.snapshotSatisfiesOperationalReadiness({}, &reason));
    QCOMPARE(reason, QStringLiteral("health_snapshot_unavailable"));

    QJsonObject snapshot;
    snapshot[QStringLiteral("requiredModelInventoryReady")] = false;
    snapshot[QStringLiteral("requiredModelInventoryReason")] =
        QStringLiteral("manifest_missing");
    QVERIFY(!manager.snapshotSatisfiesOperationalReadiness(snapshot, &reason));
    QCOMPARE(reason, QStringLiteral("manifest_missing"));

    snapshot = QJsonObject{
        {QStringLiteral("overallStatus"), QStringLiteral("healthy")},
        {QStringLiteral("queue"), QJsonObject{
             {QStringLiteral("rebuildStatus"), QStringLiteral("failed")},
             {QStringLiteral("rebuildReason"), QStringLiteral("disk_full")},
         }},
    };
    QVERIFY(!manager.snapshotSatisfiesOperationalReadiness(snapshot, &reason));
    QCOMPARE(reason, QStringLiteral("indexer_rebuild_failed:disk_full"));

    snapshot = QJsonObject{
        {QStringLiteral("overallStatus"), QStringLiteral("healthy")},
        {QStringLiteral("queue"), QJsonObject{
             {QStringLiteral("rebuildStatus"), QStringLiteral("idle")},
         }},
        {QStringLiteral("components"), QJsonObject{
             {QStringLiteral("indexer"), QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")}}},
             {QStringLiteral("query"), QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")}}},
             {QStringLiteral("extractor"), QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")}}},
             {QStringLiteral("inference"), QJsonObject{{QStringLiteral("state"), QStringLiteral("ready")}}},
         }},
        {QStringLiteral("inference"), QJsonObject{
             {QStringLiteral("connected"), true},
             {QStringLiteral("roleStatusByModel"), QJsonObject{
                  {QStringLiteral("bi-encoder"), QStringLiteral("ready")},
                  {QStringLiteral("cross-encoder"), QStringLiteral("loading")},
              }},
         }},
    };
    QVERIFY(!manager.snapshotSatisfiesOperationalReadiness(snapshot, &reason));
    QCOMPARE(reason, QStringLiteral("inference_role_unavailable:cross-encoder"));
}

void TestAppLifecycleStates::testStartWithNoServiceBinariesDoesNotCreatePhantomSession()
{
    bs::ServiceManager manager;
    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);

    manager.start();

    QVERIFY(!manager.m_started);
    QVERIFY(!manager.m_allReady);
    QCOMPARE(manager.indexerStatus(), QStringLiteral("error"));
    QCOMPARE(manager.extractorStatus(), QStringLiteral("error"));
    QCOMPARE(manager.queryStatus(), QStringLiteral("error"));
    QCOMPARE(manager.inferenceStatus(), QStringLiteral("error"));
    QCOMPARE(manager.trayState(), QStringLiteral("error"));
    QCOMPARE(errorSpy.count(), 4);
}

void TestAppLifecycleStates::testReindexPathRejectsInvalidPathBeforeBackendRequest()
{
    bs::ServiceManager manager;
    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);

    QVERIFY(!manager.reindexPath(QStringLiteral(" ../relative ")));
    QVERIFY(!manager.m_indexingActive);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("indexer"));
    QCOMPARE(errorSpy.at(0).at(1).toString(),
             QStringLiteral("Invalid path for reindex request."));
}

void TestAppLifecycleStates::testServiceActionsRejectMissingPositiveAcknowledgement()
{
    bs::ServiceManager manager;
    auto* fakeActor = new FakeControlPlaneActor();
    QThread actorThread;
    fakeActor->response = controlPlaneResponseForResult(QJsonObject{
        {QStringLiteral("cleared"), false},
        {QStringLiteral("failedPaths"), QJsonArray{QStringLiteral("/tmp/cache-entry")}},
    });
    attachFakeControlPlane(manager, fakeActor, actorThread);

    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);
    QVERIFY(!manager.clearExtractionCache());

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("extractor"));
    QCOMPARE(errorSpy.at(0).at(1).toString(),
             QStringLiteral("clearExtractionCache_failed:1_path"));
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("extractor"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("clearExtractionCache"));

    detachFakeControlPlane(manager, fakeActor, actorThread);
}

void TestAppLifecycleStates::testServiceActionsAcceptPositiveAcknowledgementAndUpdateState()
{
    bs::ServiceManager manager;
    auto* fakeActor = new FakeControlPlaneActor();
    QThread actorThread;
    fakeActor->response = controlPlaneResponseForResult(QJsonObject{
        {QStringLiteral("paused"), true},
        {QStringLiteral("queuedPaths"), 12},
    });
    attachFakeControlPlane(manager, fakeActor, actorThread);

    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);
    manager.m_indexingActive = true;
    manager.updateTrayState();

    QVERIFY(manager.pauseIndexing());
    QVERIFY(!manager.m_indexingActive);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("indexer"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("pauseIndexing"));

    detachFakeControlPlane(manager, fakeActor, actorThread);
}

void TestAppLifecycleStates::testServiceActionsReportBackendFailure()
{
    bs::ServiceManager manager;
    auto* fakeActor = new FakeControlPlaneActor();
    QThread actorThread;
    fakeActor->response = QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QStringLiteral("request_timeout")},
    };
    attachFakeControlPlane(manager, fakeActor, actorThread);

    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);
    QVERIFY(!manager.pauseIndexing());

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("indexer"));
    QCOMPARE(errorSpy.at(0).at(1).toString(), QStringLiteral("request_timeout"));
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("indexer"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("pauseIndexing"));
    QCOMPARE(fakeActor->lastTimeoutMs, 10000);

    detachFakeControlPlane(manager, fakeActor, actorThread);
}

void TestAppLifecycleStates::testServiceRequestSyncHandlesSameThreadControlPlane()
{
    bs::ServiceManager manager;
    manager.stopHealthThread();
    manager.stopControlPlaneThread();

    auto* fakeActor = new FakeControlPlaneActor();
    fakeActor->response = controlPlaneResponseForResult(QJsonObject{
        {QStringLiteral("cleared"), true},
        {QStringLiteral("removedCount"), 0},
    });
    manager.m_controlPlaneActor = fakeActor;

    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(manager.clearExtractionCache());

    QVERIFY2(elapsed.elapsed() < 200,
             qPrintable(QStringLiteral("same-thread service request blocked for %1ms")
                            .arg(elapsed.elapsed())));
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("extractor"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("clearExtractionCache"));

    manager.m_controlPlaneActor = nullptr;
    delete fakeActor;
}

void TestAppLifecycleStates::testRebuildAndReindexRequestsAcceptPositiveAcknowledgement()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir settingsDir;
    QTemporaryDir fakeHome;
    QVERIFY(settingsDir.isValid());
    QVERIFY(fakeHome.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    resetSettings();

    const QString docsPath = QDir::cleanPath(fakeHome.path() + QStringLiteral("/Documents"));
    const QJsonObject settings{
        {QStringLiteral("indexRoots"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("path"), docsPath},
                 {QStringLiteral("mode"), QStringLiteral("index_embed")},
             },
         }},
    };
    QVERIFY(writeSettings(settings));

    bs::ServiceManager manager;
    auto* fakeActor = new FakeControlPlaneActor();
    QThread actorThread;
    attachFakeControlPlane(manager, fakeActor, actorThread);

    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);

    fakeActor->response = controlPlaneResponseForResult(QJsonObject{
        {QStringLiteral("started"), true},
        {QStringLiteral("alreadyRunning"), false},
    });
    QVERIFY(manager.rebuildAll());
    QVERIFY(manager.m_indexingActive);
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("indexer"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("rebuildAll"));

    fakeActor->response = controlPlaneResponseForResult(QJsonObject{
        {QStringLiteral("started"), false},
        {QStringLiteral("alreadyRunning"), true},
        {QStringLiteral("runId"), 42},
    });
    QVERIFY(manager.rebuildVectorIndex());
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("query"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("rebuildVectorIndex"));
    QCOMPARE(fakeActor->lastParams.value(QStringLiteral("includePaths")).toArray().size(), 1);
    QCOMPARE(fakeActor->lastParams.value(QStringLiteral("includePaths"))
                 .toArray()
                 .first()
                 .toString(),
             docsPath);

    fakeActor->response = controlPlaneResponseForResult(QJsonObject{
        {QStringLiteral("queued"), true},
    });
    QVERIFY(manager.reindexPath(QUrl::fromLocalFile(docsPath).toString()));
    QCOMPARE(fakeActor->lastServiceName, QStringLiteral("indexer"));
    QCOMPARE(fakeActor->lastMethod, QStringLiteral("reindexPath"));
    QCOMPARE(fakeActor->lastParams.value(QStringLiteral("path")).toString(), docsPath);
    QCOMPARE(errorSpy.count(), 0);

    detachFakeControlPlane(manager, fakeActor, actorThread);
}

void TestAppLifecycleStates::testModelArtifactsRequireReadableFiles()
{
    QTemporaryDir modelsDir;
    QVERIFY(modelsDir.isValid());

    bs::ModelManifestEntry entry;
    entry.file = QStringLiteral("model.onnx");
    entry.vocab = QStringLiteral("vocab.txt");

    QVERIFY(!bs::ServiceManager::modelArtifactsReady(modelsDir.path(), entry));

    QFile modelFile(QDir(modelsDir.path()).filePath(entry.file));
    QVERIFY(modelFile.open(QIODevice::WriteOnly));
    QVERIFY(modelFile.write("model") > 0);
    modelFile.close();

    QVERIFY(!bs::ServiceManager::modelArtifactsReady(modelsDir.path(), entry));

    QVERIFY(QDir().mkpath(QDir(modelsDir.path()).filePath(entry.vocab)));
    QVERIFY(!bs::ServiceManager::modelArtifactsReady(modelsDir.path(), entry));
    QVERIFY(QDir(QDir(modelsDir.path()).filePath(entry.vocab)).removeRecursively());

    QFile vocabFile(QDir(modelsDir.path()).filePath(entry.vocab));
    QVERIFY(vocabFile.open(QIODevice::WriteOnly));
    QVERIFY(vocabFile.write("vocab") > 0);
    vocabFile.close();

    QVERIFY(bs::ServiceManager::modelArtifactsReady(modelsDir.path(), entry));
}

void TestAppLifecycleStates::testModelDownloadRejectsDuplicateStartAndPublishesFailure()
{
    bs::ServiceManager manager;
    QSignalSpy stateSpy(&manager, &bs::ServiceManager::modelDownloadStateChanged);

    QVERIFY(manager.downloadModels({QStringLiteral("definitely-missing-role")}, false));
    QVERIFY(manager.modelDownloadRunning());
    QCOMPARE(manager.modelDownloadStatus(),
             QStringLiteral("Preparing model download plan..."));
    QVERIFY(!manager.modelDownloadHasError());

    QVERIFY(!manager.downloadModels({QStringLiteral("bi-encoder")}, false));

    QTRY_VERIFY_WITH_TIMEOUT(!manager.modelDownloadRunning(), 5000);
    QVERIFY(manager.modelDownloadHasError());
    QVERIFY(!manager.modelDownloadStatus().isEmpty());
    QVERIFY(manager.modelDownloadStatus().contains(QStringLiteral("failed"))
            || manager.modelDownloadStatus().contains(QStringLiteral("could not load manifest")));
    QVERIFY(stateSpy.count() >= 2);
    manager.joinModelDownloadThreadIfNeeded();
}

void TestAppLifecycleStates::testStopCancelsModelDownloadThreadBeforeJoining()
{
    bs::ServiceManager manager;
    manager.m_modelDownloadCancelRequested.store(false, std::memory_order_release);
    manager.m_modelDownloadThread = std::thread([&manager]() {
        while (!manager.m_modelDownloadCancelRequested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    QElapsedTimer timer;
    timer.start();
    manager.stop();

    QVERIFY(manager.m_modelDownloadCancelRequested.load(std::memory_order_acquire));
    QVERIFY(!manager.m_modelDownloadThread.joinable());
    QVERIFY2(timer.elapsed() < 1000,
             qPrintable(QStringLiteral("ServiceManager::stop blocked for %1ms")
                            .arg(timer.elapsed())));
}

void TestAppLifecycleStates::testSingleHomeRootWithoutHomeDirectoryMapFallsBackToCuratedRoots()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir settingsDir;
    QTemporaryDir fakeHome;
    QVERIFY(settingsDir.isValid());
    QVERIFY(fakeHome.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    resetSettings();

    const QJsonObject settings{
        {QStringLiteral("indexRoots"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("path"), fakeHome.path()},
                 {QStringLiteral("mode"), QStringLiteral("index_embed")},
             },
         }},
    };
    QVERIFY(writeSettings(settings));

    bs::ServiceManager manager;
    const QJsonArray indexRoots = manager.loadIndexRoots();
    QCOMPARE(indexRoots.size(), 3);
    QVERIFY(!indexRoots.contains(fakeHome.path()));
    QVERIFY(indexRoots.contains(fakeHome.path() + QStringLiteral("/Documents")));
    QVERIFY(indexRoots.contains(fakeHome.path() + QStringLiteral("/Desktop")));
    QVERIFY(indexRoots.contains(fakeHome.path() + QStringLiteral("/Downloads")));

    const QJsonArray embeddingRoots = manager.loadEmbeddingRoots();
    QCOMPARE(embeddingRoots, indexRoots);
}

void TestAppLifecycleStates::testHomeDirectoryMapDrivesIndexAndEmbeddingRoots()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir settingsDir;
    QTemporaryDir fakeHome;
    QVERIFY(settingsDir.isValid());
    QVERIFY(fakeHome.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    resetSettings();

    const QJsonObject settings{
        {QStringLiteral("indexRoots"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("path"), fakeHome.path()},
                 {QStringLiteral("mode"), QStringLiteral("index_embed")},
             },
         }},
        {QStringLiteral("home_directories"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("Documents")},
                 {QStringLiteral("mode"), QStringLiteral("index_embed")},
             },
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("Downloads")},
                 {QStringLiteral("mode"), QStringLiteral("index")},
             },
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("Desktop")},
                 {QStringLiteral("mode"), QStringLiteral("skip")},
             },
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("   ")},
                 {QStringLiteral("mode"), QStringLiteral("index_embed")},
             },
         }},
    };
    QVERIFY(writeSettings(settings));

    bs::ServiceManager manager;
    const QJsonArray indexRoots = manager.loadIndexRoots();
    QCOMPARE(indexRoots.size(), 2);
    QVERIFY(indexRoots.contains(fakeHome.path() + QStringLiteral("/Documents")));
    QVERIFY(indexRoots.contains(fakeHome.path() + QStringLiteral("/Downloads")));
    QVERIFY(!indexRoots.contains(fakeHome.path()));
    QVERIFY(!indexRoots.contains(fakeHome.path() + QStringLiteral("/Desktop")));

    const QJsonArray embeddingRoots = manager.loadEmbeddingRoots();
    QCOMPARE(embeddingRoots.size(), 1);
    QCOMPARE(embeddingRoots.first().toString(),
             fakeHome.path() + QStringLiteral("/Documents"));
}

void TestAppLifecycleStates::testSkippedIndexRootsDoNotFallBackToCuratedRoots()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir settingsDir;
    QTemporaryDir fakeHome;
    QVERIFY(settingsDir.isValid());
    QVERIFY(fakeHome.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    resetSettings();

    const QString documents = fakeHome.path() + QStringLiteral("/Documents");
    const QString downloads = fakeHome.path() + QStringLiteral("/Downloads");
    const QJsonObject settings{
        {QStringLiteral("indexRoots"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("path"), documents},
                 {QStringLiteral("mode"), QStringLiteral("skip")},
             },
             QJsonObject{
                 {QStringLiteral("path"), downloads},
                 {QStringLiteral("mode"), QStringLiteral("index_only")},
             },
        }},
    };
    QVERIFY(writeSettings(settings));

    bs::ServiceManager manager;
    const QJsonArray indexRoots = manager.loadIndexRoots();
    QCOMPARE(indexRoots.size(), 1);
    QCOMPARE(indexRoots.first().toString(), QDir::cleanPath(downloads));
    QVERIFY(!indexRoots.contains(QDir::cleanPath(documents)));

    const QJsonArray embeddingRoots = manager.loadEmbeddingRoots();
    QVERIFY(embeddingRoots.isEmpty());

    QSignalSpy errorSpy(&manager, &bs::ServiceManager::serviceError);
    QVERIFY(!manager.rebuildVectorIndex());
    QCOMPARE(errorSpy.count(), 1);
    const QList<QVariant> args = errorSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("query"));
    QVERIFY(args.at(1).toString().contains(QStringLiteral("No embedding roots")));
}

void TestAppLifecycleStates::testIndexRootModeNormalizationMatchesSettingsContract()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir settingsDir;
    QTemporaryDir fakeHome;
    QVERIFY(settingsDir.isValid());
    QVERIFY(fakeHome.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    resetSettings();

    const QString legacyIndexOnly = fakeHome.path() + QStringLiteral("/Legacy");
    const QString missingMode = fakeHome.path() + QStringLiteral("/MissingMode");
    const QString malformedMode = fakeHome.path() + QStringLiteral("/MalformedMode");
    const QString skipped = fakeHome.path() + QStringLiteral("/Skipped");
    const QJsonObject settings{
        {QStringLiteral("indexRoots"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("path"), legacyIndexOnly},
                 {QStringLiteral("mode"), QStringLiteral("index")},
             },
             QJsonObject{
                 {QStringLiteral("path"), missingMode},
             },
             QJsonObject{
                 {QStringLiteral("path"), malformedMode},
                 {QStringLiteral("mode"), QStringLiteral(" INDEX_EMBED ")},
             },
             QJsonObject{
                 {QStringLiteral("path"), skipped},
                 {QStringLiteral("mode"), QStringLiteral("SKIP")},
             },
        }},
    };
    QVERIFY(writeSettings(settings));

    bs::ServiceManager manager;
    const QJsonArray indexRoots = manager.loadIndexRoots();
    QCOMPARE(indexRoots.size(), 3);
    QVERIFY(indexRoots.contains(QDir::cleanPath(legacyIndexOnly)));
    QVERIFY(indexRoots.contains(QDir::cleanPath(missingMode)));
    QVERIFY(indexRoots.contains(QDir::cleanPath(malformedMode)));
    QVERIFY(!indexRoots.contains(QDir::cleanPath(skipped)));

    const QJsonArray embeddingRoots = manager.loadEmbeddingRoots();
    QCOMPARE(embeddingRoots.size(), 2);
    QVERIFY(!embeddingRoots.contains(QDir::cleanPath(legacyIndexOnly)));
    QVERIFY(embeddingRoots.contains(QDir::cleanPath(missingMode)));
    QVERIFY(embeddingRoots.contains(QDir::cleanPath(malformedMode)));
}

void TestAppLifecycleStates::testOnboardingAllSkippedClearsStaleIndexRoots()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir settingsDir;
    QTemporaryDir fakeHome;
    QVERIFY(settingsDir.isValid());
    QVERIFY(fakeHome.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    resetSettings();

    const QString staleRoot = fakeHome.path() + QStringLiteral("/Documents");
    const QJsonObject staleSettings{
        {QStringLiteral("indexRoots"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("path"), staleRoot},
                 {QStringLiteral("mode"), QStringLiteral("index_embed")},
             },
        }},
    };
    QVERIFY(writeSettings(staleSettings));

    bs::OnboardingController controller;
    QVariantList directories;
    directories.append(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Documents")},
        {QStringLiteral("mode"), QStringLiteral("skip")},
    });
    directories.append(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("Downloads")},
        {QStringLiteral("mode"), QStringLiteral("skip")},
    });
    directories.append(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("../OutsideHome")},
        {QStringLiteral("mode"), QStringLiteral("index_embed")},
    });

    controller.saveHomeMap(directories);

    const QJsonObject settings = readSettings();
    QVERIFY(settings.contains(QStringLiteral("indexRoots")));
    QVERIFY(settings.value(QStringLiteral("indexRoots")).toArray().isEmpty());

    bs::ServiceManager manager;
    QVERIFY(manager.loadIndexRoots().isEmpty());
    QVERIFY(manager.loadEmbeddingRoots().isEmpty());
}

void TestAppLifecycleStates::testFdaCheckDoesNotTreatUngatedAutosaveAsGranted()
{
    EnvVarGuard homeGuard("HOME");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    QVERIFY(QDir(fakeHome.path()).mkpath(QStringLiteral("Library/Autosave Information")));
    qputenv("HOME", fakeHome.path().toUtf8());

    bs::OnboardingController controller;
    QSignalSpy fdaSpy(&controller, &bs::OnboardingController::fdaGrantedChanged);
    QSignalSpy statusSpy(&controller, &bs::OnboardingController::fdaStatusMessageChanged);

    QVERIFY(!controller.refreshFda());

    QVERIFY(!controller.fdaGranted());
    QVERIFY(controller.fdaStatusMessage().isEmpty());
    QCOMPARE(fdaSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 0);

    QVERIFY(!controller.checkFda());

    QVERIFY(!controller.fdaGranted());
    QVERIFY(controller.fdaStatusMessage().contains(QStringLiteral("Still not granted")));
    QCOMPARE(fdaSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 1);
}

void TestAppLifecycleStates::testOnboardingCompletionIsPersistedAndEmittedOnce()
{
    EnvVarGuard settingsDirGuard("BETTERSPOTLIGHT_SETTINGS_DIR");
    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    qputenv("BETTERSPOTLIGHT_SETTINGS_DIR", settingsDir.path().toUtf8());
    resetSettings();

    bs::OnboardingController controller;
    QSignalSpy needsOnboardingSpy(&controller, &bs::OnboardingController::needsOnboardingChanged);
    QSignalSpy completionSpy(&controller, &bs::OnboardingController::onboardingCompleted);

    QVERIFY(controller.needsOnboarding());

    controller.completeOnboarding();
    QVERIFY(!controller.needsOnboarding());
    QCOMPARE(needsOnboardingSpy.count(), 1);
    QCOMPARE(completionSpy.count(), 1);

    controller.completeOnboarding();
    QCOMPARE(completionSpy.count(), 1);

    bs::OnboardingController persistedController;
    QVERIFY(!persistedController.needsOnboarding());
}

QTEST_MAIN(TestAppLifecycleStates)
#include "test_app_lifecycle_states.moc"
