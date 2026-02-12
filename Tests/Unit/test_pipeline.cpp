#include <QtTest/QtTest>

#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"
#include "core/index/sqlite_store.h"
#include "core/indexing/pipeline.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>

class TestPipeline : public QObject {
    Q_OBJECT

private slots:
    void testLifecycleAndBehaviorPaths();
    void testTransientExtractionFailureTriggersBoundedRetriesWithBackoff();
};

void TestPipeline::testLifecycleAndBehaviorPaths()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString rootPath = QDir(tempDir.path()).filePath(QStringLiteral("root"));
    QVERIFY(QDir().mkpath(rootPath));
    const QString filePath = QDir(rootPath).filePath(QStringLiteral("fixture.txt"));
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("pipeline fixture content for repeated indexing\n");
        f.close();
    }

    bs::ExtractionManager extractor;
    bs::PathRules rules;
    rules.setExplicitIncludeRoots({rootPath.toStdString()});

    std::atomic<int> rssMb{64};
    bs::PipelineRuntimeConfig cfg;
    cfg.batchCommitSize = 4;
    cfg.batchCommitIntervalMs = 15;
    cfg.enqueueRetrySleepMs = 2;
    cfg.memoryPressureSleepMs = 2;
    cfg.drainPollAttempts = 200;
    cfg.drainPollIntervalMs = 10;
    cfg.rssProvider = [&rssMb]() { return rssMb.load(); };

    bs::Pipeline pipeline(store, extractor, rules, cfg);
    pipeline.start({rootPath.toStdString()});

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000 && pipeline.processedCount() < 1) {
        QTest::qWait(20);
    }
    QVERIFY2(pipeline.processedCount() >= 1, "Initial scan should process at least one item");

    pipeline.pause();
    QVERIFY(pipeline.queueStatus().isPaused);
    pipeline.resume();
    QVERIFY(!pipeline.queueStatus().isPaused);

    const int processedBeforeReindex = pipeline.processedCount();
    pipeline.reindexPath(filePath);
    timer.restart();
    while (timer.elapsed() < 8000 && pipeline.processedCount() <= processedBeforeReindex) {
        QTest::qWait(20);
    }
    QVERIFY(pipeline.processedCount() > processedBeforeReindex);

    for (int i = 0; i < 80; ++i) {
        pipeline.reindexPath(filePath);
    }
    timer.restart();
    bool sawCoalesced = false;
    bool sawStaleDropped = false;
    while (timer.elapsed() < 8000) {
        const bs::QueueStats stats = pipeline.queueStatus();
        sawCoalesced = sawCoalesced || stats.coalesced > 0;
        sawStaleDropped = sawStaleDropped || stats.staleDropped > 0;
        if (sawCoalesced && sawStaleDropped) {
            break;
        }
        QTest::qWait(20);
    }
    QVERIFY2(sawCoalesced, "Expected coordinator coalescing under repeated same-path reindex");
    QVERIFY2(sawStaleDropped, "Expected stale prepared work to be dropped");

    rssMb.store(4096);
    pipeline.setUserActive(true);
    QVERIFY(pipeline.queueStatus().prepWorkers == 1);
    pipeline.setUserActive(false);
    QVERIFY(pipeline.queueStatus().prepWorkers == 1);

    rssMb.store(64);
    pipeline.setUserActive(true);
    QVERIFY(pipeline.queueStatus().prepWorkers == 1);
    pipeline.setUserActive(false);
    QVERIFY(pipeline.queueStatus().prepWorkers >= 2);

    pipeline.rebuildAll({rootPath.toStdString()});
    timer.restart();
    bool drainedAfterRebuild = false;
    while (timer.elapsed() < 12000) {
        const bs::QueueStats stats = pipeline.queueStatus();
        if (stats.depth == 0 && stats.preparing == 0 && stats.writing == 0) {
            drainedAfterRebuild = true;
            break;
        }
        QTest::qWait(25);
    }
    QVERIFY2(drainedAfterRebuild, "Pipeline should drain after rebuildAll");

    pipeline.stop();
    pipeline.stop();
}

void TestPipeline::testTransientExtractionFailureTriggersBoundedRetriesWithBackoff()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString rootPath = QDir(tempDir.path()).filePath(QStringLiteral("root"));
    QVERIFY(QDir().mkpath(rootPath));

    const QString transientPath = QDir(rootPath).filePath(QStringLiteral("transient.doc"));
    QFile::remove(transientPath);

    bs::ExtractionManager extractor;
    bs::PathRules rules;
    rules.setExplicitIncludeRoots({rootPath.toStdString()});

    bs::PipelineRuntimeConfig cfg;
    cfg.batchCommitSize = 1;
    cfg.batchCommitIntervalMs = 10;
    cfg.maxPipelineRetries = 2;
    cfg.enqueueRetrySleepMs = 2;
    cfg.memoryPressureSleepMs = 2;
    cfg.drainPollAttempts = 250;
    cfg.drainPollIntervalMs = 10;
    cfg.retryBackoffBaseMs = 50;
    cfg.retryBackoffCapMs = 100;
    cfg.rssProvider = []() { return 64; };

    bs::Pipeline pipeline(store, extractor, rules, cfg);
    pipeline.start({});

    QVERIFY2(QFile::link(QStringLiteral("/dev/null"), transientPath),
             "Failed to create test symlink fixture");

    QElapsedTimer timer;
    timer.start();
    const int processedBefore = pipeline.processedCount();

    pipeline.reindexPath(transientPath);

    bool observedRetrySettlement = false;
    while (timer.elapsed() < 12000) {
        const bs::QueueStats stats = pipeline.queueStatus();
        if (pipeline.processedCount() >= processedBefore + 2
            && stats.depth == 0
            && stats.preparing == 0
            && stats.writing == 0) {
            observedRetrySettlement = true;
            break;
        }
        QTest::qWait(20);
    }

    QVERIFY2(observedRetrySettlement,
             "Expected transient extraction failure to retry and settle cleanly");
    QVERIFY2(timer.elapsed() >= 40,
             "Retry path should include backoff delay before terminal failure");

    pipeline.stop();
}

QTEST_MAIN(TestPipeline)
#include "test_pipeline.moc"
