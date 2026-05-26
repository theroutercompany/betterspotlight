#include <QtTest/QtTest>

#define private public
#include "core/fs/file_monitor_macos.h"
#undef private

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <cmath>
#include <limits>

class TestFileMonitorMacOS : public QObject {
    Q_OBJECT

private slots:
    void testConstructorNormalizesLatency();
    void testStartRejectsInvalidRootsBeforeCreatingStream();
    void testClassifyEventPriority();
    void testHandleEventsRejectsMalformedBatch();
    void testHandleEventsSkipsMissingPathAndFlushesValidEvent();
    void testStopDrainsScheduledDebounceFlush();
};

void TestFileMonitorMacOS::testConstructorNormalizesLatency()
{
    bs::FileMonitorMacOS nanLatency(std::numeric_limits<double>::quiet_NaN());
    QCOMPARE(nanLatency.m_latency, 0.5);

    bs::FileMonitorMacOS negativeLatency(-1.0);
    QCOMPARE(negativeLatency.m_latency, 0.5);

    bs::FileMonitorMacOS tinyLatency(0.001);
    QCOMPARE(tinyLatency.m_latency, 0.05);

    bs::FileMonitorMacOS hugeLatency(600.0);
    QCOMPARE(hugeLatency.m_latency, 60.0);

    bs::FileMonitorMacOS normalLatency(0.25);
    QCOMPARE(normalLatency.m_latency, 0.25);
}

void TestFileMonitorMacOS::testStartRejectsInvalidRootsBeforeCreatingStream()
{
    bs::FileMonitorMacOS monitor;
    auto callback = [](const std::vector<bs::WorkItem>&) {};

    QVERIFY(!monitor.start({std::string()}, callback));
    QVERIFY(!monitor.isRunning());
    QVERIFY(!monitor.m_callback);
    QVERIFY(monitor.m_roots.empty());

    std::string embeddedNul("prefix\0suffix", 13);
    QVERIFY(!monitor.start({embeddedNul}, callback));
    QVERIFY(!monitor.isRunning());
    QVERIFY(!monitor.m_callback);
    QVERIFY(monitor.m_roots.empty());
}

void TestFileMonitorMacOS::testClassifyEventPriority()
{
    using Type = bs::WorkItem::Type;

    QCOMPARE(static_cast<int>(bs::FileMonitorMacOS::classifyEvent(
                 kFSEventStreamEventFlagItemRemoved)),
             static_cast<int>(Type::Delete));
    QCOMPARE(static_cast<int>(bs::FileMonitorMacOS::classifyEvent(
                 kFSEventStreamEventFlagItemRenamed)),
             static_cast<int>(Type::Delete));
    QCOMPARE(static_cast<int>(bs::FileMonitorMacOS::classifyEvent(
                 kFSEventStreamEventFlagItemRenamed |
                 kFSEventStreamEventFlagItemCreated)),
             static_cast<int>(Type::NewFile));
    QCOMPARE(static_cast<int>(bs::FileMonitorMacOS::classifyEvent(
                 kFSEventStreamEventFlagItemCreated)),
             static_cast<int>(Type::NewFile));
    QCOMPARE(static_cast<int>(bs::FileMonitorMacOS::classifyEvent(
                 kFSEventStreamEventFlagItemInodeMetaMod)),
             static_cast<int>(Type::ModifiedContent));
    QCOMPARE(static_cast<int>(bs::FileMonitorMacOS::classifyEvent(0)),
             static_cast<int>(Type::ModifiedContent));
}

void TestFileMonitorMacOS::testHandleEventsRejectsMalformedBatch()
{
    bs::FileMonitorMacOS monitor;
    monitor.m_running.store(true);

    int callbackCount = 0;
    int errorCount = 0;
    {
        std::lock_guard<std::mutex> lock(monitor.m_mutex);
        monitor.m_callback = [&](const std::vector<bs::WorkItem>&) {
            ++callbackCount;
        };
    }
    monitor.setErrorCallback([&](const QString& error) {
        if (error.contains(QStringLiteral("malformed callback batch"))) {
            ++errorCount;
        }
    });

    const FSEventStreamEventFlags flags[] = {kFSEventStreamEventFlagItemModified};
    const FSEventStreamEventId ids[] = {7};
    monitor.handleEvents(1, nullptr, flags, ids);

    QCOMPARE(callbackCount, 0);
    QCOMPARE(errorCount, 1);
    QCOMPARE(monitor.lastEventId(), static_cast<uint64_t>(kFSEventStreamEventIdSinceNow));
}

void TestFileMonitorMacOS::testHandleEventsSkipsMissingPathAndFlushesValidEvent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath(QStringLiteral("valid.txt"));
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("fixture");
        file.close();
    }

    bs::FileMonitorMacOS monitor;
    monitor.m_running.store(true);

    int missingPathErrors = 0;
    std::vector<bs::WorkItem> observed;
    {
        std::lock_guard<std::mutex> lock(monitor.m_mutex);
        monitor.m_callback = [&](const std::vector<bs::WorkItem>& batch) {
            observed = batch;
        };
    }
    monitor.setErrorCallback([&](const QString& error) {
        if (error.contains(QStringLiteral("event missing path"))) {
            ++missingPathErrors;
        }
    });

    QByteArray pathUtf8 = filePath.toUtf8();
    char* paths[] = {nullptr, pathUtf8.data()};
    const FSEventStreamEventFlags flags[] = {
        kFSEventStreamEventFlagItemModified,
        kFSEventStreamEventFlagItemModified
    };
    const FSEventStreamEventId ids[] = {11, 12};

    monitor.handleEvents(2, paths, flags, ids);

    QCOMPARE(missingPathErrors, 1);
    QCOMPARE(observed.size(), size_t(1));
    QCOMPARE(QString::fromStdString(observed.front().filePath), filePath);
    QCOMPARE(static_cast<int>(observed.front().type),
             static_cast<int>(bs::WorkItem::Type::ModifiedContent));
    QVERIFY(observed.front().knownSize.has_value());
    QCOMPARE(*observed.front().knownSize, static_cast<uint64_t>(7));
    QCOMPARE(monitor.lastEventId(), uint64_t{12});
}

void TestFileMonitorMacOS::testStopDrainsScheduledDebounceFlush()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath(QStringLiteral("watched.txt"));
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("fixture");
        file.close();
    }

    bs::FileMonitorMacOS monitor;
    std::atomic<int> callbackCount{0};

    monitor.m_running.store(true);
    monitor.m_queue = dispatch_queue_create("com.betterspotlight.file-monitor-test",
                                            DISPATCH_QUEUE_SERIAL);
    QVERIFY(monitor.m_queue != nullptr);
    {
        std::lock_guard<std::mutex> lock(monitor.m_mutex);
        monitor.m_callback = [&](const std::vector<bs::WorkItem>& batch) {
            if (!batch.empty()) {
                callbackCount.fetch_add(1);
            }
        };
    }

    QByteArray pathUtf8 = filePath.toUtf8();
    char* paths[] = {pathUtf8.data()};
    const FSEventStreamEventFlags flags[] = {
        kFSEventStreamEventFlagItemModified
    };
    const FSEventStreamEventId ids[] = {1};

    monitor.handleEvents(1, paths, flags, ids);
    QCOMPARE(monitor.m_pendingFlushTasks, size_t(1));

    QElapsedTimer elapsed;
    elapsed.start();
    monitor.stop();

    QVERIFY2(elapsed.elapsed() >= bs::FileMonitorMacOS::kDebounceMs - 50,
             "stop() should wait for scheduled debounce flush blocks");
    QCOMPARE(monitor.m_pendingFlushTasks, size_t(0));
    QCOMPARE(callbackCount.load(), 1);
}

QTEST_MAIN(TestFileMonitorMacOS)
#include "test_file_monitor_macos.moc"
