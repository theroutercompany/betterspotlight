#include <QtTest/QtTest>

#define private public
#include "app/onboarding_controller.h"
#include "app/service_manager.h"
#undef private

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>

namespace {

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/settings.json");
}

void resetSettings()
{
    QFile::remove(settingsPath());
}

} // namespace

class TestAppLifecycleStates : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void testTrayStateTransitions();
    void testInitialIndexingTriggerIsGatedAndSingleShot();
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

void TestAppLifecycleStates::testOnboardingCompletionIsPersistedAndEmittedOnce()
{
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
