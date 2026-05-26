#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QFileInfo>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QtQuickControls2/QQuickStyle>

namespace {

class OnboardingControllerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool needsOnboarding READ needsOnboarding NOTIFY needsOnboardingChanged)
    Q_PROPERTY(bool fdaGranted READ fdaGranted NOTIFY fdaGrantedChanged)
    Q_PROPERTY(QString fdaStatusMessage READ fdaStatusMessage NOTIFY fdaStatusMessageChanged)
    Q_PROPERTY(QVariantList homeDirectories READ homeDirectories NOTIFY homeDirectoriesChanged)

public:
    bool needsOnboarding() const { return true; }
    bool fdaGranted() const { return m_fdaGranted; }
    QString fdaStatusMessage() const { return m_fdaStatusMessage; }

    QVariantList homeDirectories() const
    {
        return {
            QVariantMap{
                {QStringLiteral("name"), QStringLiteral("Documents")},
                {QStringLiteral("icon"), QStringLiteral("D")},
                {QStringLiteral("suggestedMode"), QStringLiteral("index_embed")},
            },
        };
    }

    Q_INVOKABLE bool refreshFda() { return m_fdaGranted; }
    Q_INVOKABLE bool checkFda() { return m_fdaGranted; }
    Q_INVOKABLE void openFdaSystemSettings() {}
    Q_INVOKABLE void saveHomeMap(const QVariantList&) {}
    Q_INVOKABLE void completeOnboarding() { emit onboardingCompleted(); }

    void setFdaGranted(bool granted)
    {
        if (m_fdaGranted == granted) {
            return;
        }
        m_fdaGranted = granted;
        m_fdaStatusMessage = granted ? QStringLiteral("Access granted") : QStringLiteral("Not yet granted");
        emit fdaGrantedChanged();
        emit fdaStatusMessageChanged();
    }

signals:
    void needsOnboardingChanged();
    void fdaGrantedChanged();
    void fdaStatusMessageChanged();
    void homeDirectoriesChanged();
    void onboardingCompleted();

private:
    bool m_fdaGranted = false;
    QString m_fdaStatusMessage = QStringLiteral("Not yet granted");
};

class ServiceManagerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool modelDownloadRunning READ modelDownloadRunning NOTIFY modelDownloadStateChanged)
    Q_PROPERTY(QString modelDownloadStatus READ modelDownloadStatus NOTIFY modelDownloadStateChanged)
    Q_PROPERTY(bool modelDownloadHasError READ modelDownloadHasError NOTIFY modelDownloadStateChanged)

public:
    bool modelDownloadRunning() const { return m_modelDownloadRunning; }
    QString modelDownloadStatus() const { return m_modelDownloadStatus; }
    bool modelDownloadHasError() const { return m_modelDownloadHasError; }

    Q_INVOKABLE bool downloadModels(const QStringList&, bool) { return true; }

    void setModelDownloadState(bool running, const QString& status, bool hasError)
    {
        m_modelDownloadRunning = running;
        m_modelDownloadStatus = status;
        m_modelDownloadHasError = hasError;
        emit modelDownloadStateChanged();
    }

signals:
    void modelDownloadStateChanged();

private:
    bool m_modelDownloadRunning = false;
    QString m_modelDownloadStatus = QStringLiteral("No downloads started yet.");
    bool m_modelDownloadHasError = false;
};

void visitObjectTree(QObject* object, const std::function<void(QObject*)>& visit, QSet<QObject*>* seen)
{
    if (object == nullptr || seen->contains(object)) {
        return;
    }

    seen->insert(object);
    visit(object);

    for (QObject* child : object->children()) {
        visitObjectTree(child, visit, seen);
    }

    auto* item = qobject_cast<QQuickItem*>(object);
    if (item == nullptr) {
        return;
    }

    for (QQuickItem* childItem : item->childItems()) {
        visitObjectTree(childItem, visit, seen);
    }
}

QObject* findObjectWithText(QObject* root, const QString& text)
{
    QObject* match = nullptr;
    QSet<QObject*> seen;
    visitObjectTree(root,
                    [&](QObject* object) {
                        if (match == nullptr && object->property("text").toString() == text) {
                            match = object;
                        }
                    },
                    &seen);
    return match;
}

QObject* findObjectByName(QObject* root, const QString& objectName)
{
    QObject* match = nullptr;
    QSet<QObject*> seen;
    visitObjectTree(root,
                    [&](QObject* object) {
                        if (match == nullptr && object->objectName() == objectName) {
                            match = object;
                        }
                    },
                    &seen);
    return match;
}

QRectF itemRectInWindow(QQuickWindow* window, QQuickItem* item)
{
    const QPointF topLeft = item->mapToItem(window->contentItem(), QPointF(0, 0));
    return QRectF(topLeft, QSizeF(item->width(), item->height()));
}

void resizeWindowForLayoutStress(QQuickWindow* window, int height = 440)
{
    window->setMinimumHeight(height);
    window->resize(window->width(), height);
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCOMPARE(window->height(), height);
}

void addQmlFilePathRows()
{
    QTest::addColumn<QString>("qmlFilePath");

    QTest::newRow("source-qml")
        << (QStringLiteral(BETTERSPOTLIGHT_SOURCE_DIR)
            + QStringLiteral("/src/app/qml/onboarding/OnboardingWindow.qml"));
    QTest::newRow("built-qml")
        << (QStringLiteral(BETTERSPOTLIGHT_BUILD_QML_DIR)
            + QStringLiteral("/onboarding/OnboardingWindow.qml"));
}

} // namespace

class TestOnboardingQmlLayout : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testFdaStepFooterFitsFixedWindow_data();
    void testFdaStepFooterFitsFixedWindow();
    void testModelStepFooterFitsFixedWindow_data();
    void testModelStepFooterFitsFixedWindow();
};

void TestOnboardingQmlLayout::initTestCase()
{
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

void TestOnboardingQmlLayout::testModelStepFooterFitsFixedWindow_data()
{
    addQmlFilePathRows();
}

void TestOnboardingQmlLayout::testFdaStepFooterFitsFixedWindow_data()
{
    addQmlFilePathRows();
}

void TestOnboardingQmlLayout::testFdaStepFooterFitsFixedWindow()
{
    QFETCH(QString, qmlFilePath);
    QVERIFY2(QFileInfo::exists(qmlFilePath),
             qPrintable(QStringLiteral("QML file was not present: %1").arg(qmlFilePath)));

    OnboardingControllerStub onboarding;
    ServiceManagerStub serviceManager;

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("onboardingControllerObj"), &onboarding);
    engine.rootContext()->setContextProperty(QStringLiteral("serviceManagerObj"), &serviceManager);

    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlFilePath));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(!root.isNull(), qPrintable(component.errorString()));

    auto* window = qobject_cast<QQuickWindow*>(root.data());
    QVERIFY(window != nullptr);
    QCOMPARE(window->width(), 580);
    QCOMPARE(window->height(), 500);

    QVERIFY(window->setProperty("currentStep", 1));
    window->show();
    QCoreApplication::processEvents();
    QTest::qWait(50);

    QObject* fdaStepObject = findObjectByName(window, QStringLiteral("fdaStep"));
    QVERIFY2(fdaStepObject != nullptr, "FDA step was not present");
    QVERIFY2(findObjectWithText(window, QStringLiteral("2. BetterSpotlight should already be listed"))
                 != nullptr,
             "FDA instructions did not explain that the current app should be pre-listed");
    QVERIFY2(findObjectWithText(
                 window,
                 QStringLiteral("BetterSpotlight is registered before System Settings opens. "
                                "Use '+' only as a fallback if macOS still does not show it."))
                 != nullptr,
             "FDA instructions did not reserve '+' for the fallback path");

    QObject* footerObject = findObjectByName(window, QStringLiteral("fdaFooter"));
    QVERIFY2(footerObject != nullptr, "FDA footer was not present");
    auto* footer = qobject_cast<QQuickItem*>(footerObject);
    QVERIFY(footer != nullptr);
    QVERIFY(footer->isVisible());

    QObject* skipObject = findObjectByName(window, QStringLiteral("fdaSkipButton"));
    QVERIFY2(skipObject != nullptr, "FDA skip button item was not present");
    auto* skipButton = qobject_cast<QQuickItem*>(skipObject);
    QVERIFY(skipButton != nullptr);
    QVERIFY(skipButton->isVisible());

    QObject* verifyObject = findObjectByName(window, QStringLiteral("fdaVerifyButton"));
    QVERIFY2(verifyObject != nullptr, "FDA verify button item was not present");
    auto* verifyButton = qobject_cast<QQuickItem*>(verifyObject);
    QVERIFY(verifyButton != nullptr);
    QVERIFY(verifyButton->isVisible());

    QSignalSpy nextSpy(fdaStepObject, SIGNAL(next()));
    QVERIFY(nextSpy.isValid());
    QTest::mouseClick(window,
                      Qt::LeftButton,
                      Qt::NoModifier,
                      itemRectInWindow(window, verifyButton).center().toPoint());
    QCoreApplication::processEvents();
    QCOMPARE(nextSpy.count(), 0);

    resizeWindowForLayoutStress(window);

    QObject* scrollObject = findObjectByName(window, QStringLiteral("fdaBodyScroll"));
    QVERIFY2(scrollObject != nullptr, "FDA body scroll view was not present");
    auto* bodyScroll = qobject_cast<QQuickItem*>(scrollObject);
    QVERIFY(bodyScroll != nullptr);
    QVERIFY(bodyScroll->isVisible());

    QRectF buttonRect = itemRectInWindow(window, skipButton);
    QRectF footerRect = itemRectInWindow(window, footer);
    QRectF scrollRect = itemRectInWindow(window, bodyScroll);
    const QRectF windowRect(QPointF(0, 0), QSizeF(window->width(), window->height()));

    QVERIFY2(windowRect.contains(buttonRect),
             qPrintable(QStringLiteral("skip button rect=%1,%2 %3x%4 window=%5x%6")
                            .arg(buttonRect.x())
                            .arg(buttonRect.y())
                            .arg(buttonRect.width())
                            .arg(buttonRect.height())
                            .arg(window->width())
                            .arg(window->height())));
    QVERIFY2(windowRect.contains(footerRect),
             qPrintable(QStringLiteral("FDA footer rect=%1,%2 %3x%4 window=%5x%6")
                            .arg(footerRect.x())
                            .arg(footerRect.y())
                            .arg(footerRect.width())
                            .arg(footerRect.height())
                            .arg(window->width())
                            .arg(window->height())));
    QVERIFY2(scrollRect.bottom() <= footerRect.top(),
             qPrintable(QStringLiteral("FDA scroll bottom=%1 footer top=%2")
                            .arg(scrollRect.bottom())
                            .arg(footerRect.top())));
    QVERIFY2(window->height() - footerRect.bottom() >= 20.0,
             qPrintable(QStringLiteral("FDA footer bottom margin=%1 footer rect=%2,%3 %4x%5 window=%6x%7")
                            .arg(window->height() - footerRect.bottom())
                            .arg(footerRect.x())
                            .arg(footerRect.y())
                            .arg(footerRect.width())
                            .arg(footerRect.height())
                            .arg(window->width())
                            .arg(window->height())));

    onboarding.setFdaGranted(true);
    QCoreApplication::processEvents();
    QTest::qWait(50);

    QObject* continueObject = findObjectByName(window, QStringLiteral("fdaContinueButton"));
    QVERIFY2(continueObject != nullptr, "FDA continue button item was not present");
    auto* continueButton = qobject_cast<QQuickItem*>(continueObject);
    QVERIFY(continueButton != nullptr);
    QVERIFY(continueButton->isVisible());

    buttonRect = itemRectInWindow(window, continueButton);
    footerRect = itemRectInWindow(window, footer);
    QVERIFY2(windowRect.contains(buttonRect),
             qPrintable(QStringLiteral("continue button rect=%1,%2 %3x%4 window=%5x%6")
                            .arg(buttonRect.x())
                            .arg(buttonRect.y())
                            .arg(buttonRect.width())
                            .arg(buttonRect.height())
                            .arg(window->width())
                            .arg(window->height())));
    QVERIFY2(window->height() - footerRect.bottom() >= 20.0,
             qPrintable(QStringLiteral("FDA granted footer bottom margin=%1 footer rect=%2,%3 %4x%5 window=%6x%7")
                            .arg(window->height() - footerRect.bottom())
                            .arg(footerRect.x())
                            .arg(footerRect.y())
                            .arg(footerRect.width())
                            .arg(footerRect.height())
                            .arg(window->width())
                            .arg(window->height())));

    QTest::mouseClick(window,
                      Qt::LeftButton,
                      Qt::NoModifier,
                      itemRectInWindow(window, verifyButton).center().toPoint());
    QCoreApplication::processEvents();
    QCOMPARE(nextSpy.count(), 1);
}

void TestOnboardingQmlLayout::testModelStepFooterFitsFixedWindow()
{
    QFETCH(QString, qmlFilePath);
    QVERIFY2(QFileInfo::exists(qmlFilePath),
             qPrintable(QStringLiteral("QML file was not present: %1").arg(qmlFilePath)));

    OnboardingControllerStub onboarding;
    ServiceManagerStub serviceManager;

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("onboardingControllerObj"), &onboarding);
    engine.rootContext()->setContextProperty(QStringLiteral("serviceManagerObj"), &serviceManager);

    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlFilePath));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(!root.isNull(), qPrintable(component.errorString()));

    auto* window = qobject_cast<QQuickWindow*>(root.data());
    QVERIFY(window != nullptr);
    QCOMPARE(window->width(), 580);
    QCOMPARE(window->height(), 500);

    QVERIFY(window->setProperty("currentStep", 3));
    window->show();
    QCoreApplication::processEvents();
    QTest::qWait(50);

    QObject* modelStepObject = findObjectByName(window, QStringLiteral("modelSetupStep"));
    QVERIFY2(modelStepObject != nullptr, "Model setup step was not present");
    QVERIFY(modelStepObject->setProperty("downloadStarted", true));
    serviceManager.setModelDownloadState(
        false, QStringLiteral("Model download complete: 0 downloaded, 5 skipped"), false);
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QVERIFY(modelStepObject->property("downloadCompleted").toBool());
    QVERIFY(modelStepObject->property("downloadSucceeded").toBool());

    resizeWindowForLayoutStress(window);

    QVERIFY2(findObjectWithText(window, QStringLiteral("Finish Setup")) != nullptr,
             "Finish Setup text was not present in the model setup step");

    QObject* finishObject = findObjectByName(window, QStringLiteral("modelSetupFinishButton"));
    QVERIFY2(finishObject != nullptr, "Finish Setup button item was not present in the model setup step");

    auto* finishButton = qobject_cast<QQuickItem*>(finishObject);
    QVERIFY(finishButton != nullptr);
    QVERIFY(finishButton->isVisible());
    QVERIFY(finishButton->width() > 0);
    QVERIFY(finishButton->height() > 0);

    QObject* footerObject = findObjectByName(window, QStringLiteral("modelSetupFooter"));
    QVERIFY2(footerObject != nullptr, "Model setup footer was not present");
    auto* footer = qobject_cast<QQuickItem*>(footerObject);
    QVERIFY(footer != nullptr);
    QVERIFY(footer->isVisible());

    QObject* scrollObject = findObjectByName(window, QStringLiteral("modelSetupBodyScroll"));
    QVERIFY2(scrollObject != nullptr, "Model setup body scroll view was not present");
    auto* bodyScroll = qobject_cast<QQuickItem*>(scrollObject);
    QVERIFY(bodyScroll != nullptr);
    QVERIFY(bodyScroll->isVisible());

    QObject* contentObject = findObjectByName(window, QStringLiteral("modelSetupBodyContent"));
    QVERIFY2(contentObject != nullptr, "Model setup body content was not present");
    auto* bodyContent = qobject_cast<QQuickItem*>(contentObject);
    QVERIFY(bodyContent != nullptr);
    QVERIFY2(bodyContent->height() >= bodyScroll->height(),
             qPrintable(QStringLiteral("body content height=%1 scroll height=%2")
                            .arg(bodyContent->height())
                            .arg(bodyScroll->height())));

    const QRectF buttonRect = itemRectInWindow(window, finishButton);
    const QRectF footerRect = itemRectInWindow(window, footer);
    const QRectF scrollRect = itemRectInWindow(window, bodyScroll);
    const QRectF windowRect(QPointF(0, 0), QSizeF(window->width(), window->height()));

    QVERIFY2(windowRect.contains(buttonRect),
             qPrintable(QStringLiteral("button rect=%1,%2 %3x%4 window=%5x%6")
                            .arg(buttonRect.x())
                            .arg(buttonRect.y())
                            .arg(buttonRect.width())
                            .arg(buttonRect.height())
                            .arg(window->width())
                            .arg(window->height())));
    QVERIFY2(windowRect.contains(footerRect),
             qPrintable(QStringLiteral("footer rect=%1,%2 %3x%4 window=%5x%6")
                            .arg(footerRect.x())
                            .arg(footerRect.y())
                            .arg(footerRect.width())
                            .arg(footerRect.height())
                            .arg(window->width())
                            .arg(window->height())));
    QVERIFY2(scrollRect.bottom() <= footerRect.top(),
             qPrintable(QStringLiteral("scroll bottom=%1 footer top=%2")
                            .arg(scrollRect.bottom())
                            .arg(footerRect.top())));
    QVERIFY2(window->height() - buttonRect.bottom() >= 20.0,
             qPrintable(QStringLiteral("button bottom margin=%1 button rect=%2,%3 %4x%5 window=%6x%7")
                            .arg(window->height() - buttonRect.bottom())
                            .arg(buttonRect.x())
                            .arg(buttonRect.y())
                            .arg(buttonRect.width())
                            .arg(buttonRect.height())
                            .arg(window->width())
                            .arg(window->height())));
    QVERIFY2(window->height() - footerRect.bottom() >= 20.0,
             qPrintable(QStringLiteral("footer bottom margin=%1 footer rect=%2,%3 %4x%5 window=%6x%7")
                            .arg(window->height() - footerRect.bottom())
                            .arg(footerRect.x())
                            .arg(footerRect.y())
                            .arg(footerRect.width())
                            .arg(footerRect.height())
                            .arg(window->width())
                            .arg(window->height())));
}

QTEST_MAIN(TestOnboardingQmlLayout)
#include "test_onboarding_qml_layout.moc"
