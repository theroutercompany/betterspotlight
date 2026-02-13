#include "hotkey_manager.h"
#include "onboarding_controller.h"
#include "search_controller.h"
#include "service_manager.h"
#include "settings_controller.h"
#include "status_bar_bridge.h"
#include "system_interaction_collector.h"
#include "runtime_environment.h"
#include "update_manager.h"
#include "core/models/model_registry.h"
#include "core/shared/logging.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLockFile>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QtGui/QStyleHints>
#include <QSystemTrayIcon>

namespace {

enum class TrayGlyphVariant {
    Idle,
    IndexingA,
    IndexingB,
    Error,
};

class TrayStateController : public QObject {
public:
    TrayStateController(QSystemTrayIcon* trayIcon,
                        bs::ServiceManager* serviceManager,
                        const QIcon& idleIcon,
                        const QIcon& indexingIconA,
                        const QIcon& indexingIconB,
                        const QIcon& errorIcon,
                        QObject* parent = nullptr)
        : QObject(parent)
        , m_trayIcon(trayIcon)
        , m_serviceManager(serviceManager)
        , m_idleIcon(idleIcon)
        , m_indexingIconA(indexingIconA)
        , m_indexingIconB(indexingIconB)
        , m_errorIcon(errorIcon)
    {
        m_pulseTimer.setInterval(700);
        connect(&m_pulseTimer, &QTimer::timeout,
                this, &TrayStateController::updatePresentation);
        connect(m_serviceManager, &bs::ServiceManager::trayStateChanged,
                this, &TrayStateController::updatePresentation);
        connect(m_serviceManager, &bs::ServiceManager::serviceStatusChanged,
                this, &TrayStateController::updatePresentation);
    }

    void quiesce()
    {
        m_stopping = true;
        m_pulseTimer.stop();
    }

    void updatePresentation()
    {
        if (m_stopping || !m_trayIcon || !m_serviceManager) {
            return;
        }

        const QString state = m_serviceManager->trayState();
        if (state == QLatin1String("error")) {
            m_pulseTimer.stop();
            m_trayIcon->setIcon(m_errorIcon);
            m_trayIcon->setToolTip(
                QStringLiteral("BetterSpotlight - Error (click to open Index Health)"));
            return;
        }
        if (state == QLatin1String("indexing")) {
            m_trayIcon->setIcon(m_pulseFlip ? m_indexingIconA : m_indexingIconB);
            m_pulseFlip = !m_pulseFlip;
            m_trayIcon->setToolTip(
                QStringLiteral("BetterSpotlight - Indexing in progress (click to open Index Health)"));
            if (!m_pulseTimer.isActive()) {
                m_pulseTimer.start();
            }
            return;
        }

        m_pulseTimer.stop();
        m_trayIcon->setIcon(m_idleIcon);
        m_trayIcon->setToolTip(
            QStringLiteral("BetterSpotlight - Ready (idle, click to open Index Health)"));
    }

private:
    QPointer<QSystemTrayIcon> m_trayIcon;
    QPointer<bs::ServiceManager> m_serviceManager;
    QIcon m_idleIcon;
    QIcon m_indexingIconA;
    QIcon m_indexingIconB;
    QIcon m_errorIcon;
    QTimer m_pulseTimer;
    bool m_pulseFlip = false;
    bool m_stopping = false;
};

QIcon fallbackTrayStateIcon(TrayGlyphVariant variant)
{
    constexpr int kSize = 24;
    QPixmap pixmap(kSize, kSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink(255, 255, 255, 235);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 2.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QRectF(4.2, 4.2, 11.0, 11.0));
    painter.drawLine(QPointF(13.2, 13.2), QPointF(19.4, 19.4));

    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    if (variant == TrayGlyphVariant::Error) {
        painter.drawRoundedRect(QRectF(17.2, 3.5, 2.5, 8.2), 1.0, 1.0);
        painter.drawEllipse(QRectF(17.2, 14.0, 2.5, 2.5));
    } else if (variant == TrayGlyphVariant::IndexingA
               || variant == TrayGlyphVariant::IndexingB) {
        const qreal cy = (variant == TrayGlyphVariant::IndexingA) ? 5.0 : 8.0;
        painter.drawEllipse(QRectF(17.2, cy, 3.1, 3.1));
    } else {
        QPolygonF sparkle;
        sparkle << QPointF(18.6, 3.6)
                << QPointF(19.6, 6.6)
                << QPointF(22.6, 7.6)
                << QPointF(19.6, 8.6)
                << QPointF(18.6, 11.6)
                << QPointF(17.6, 8.6)
                << QPointF(14.6, 7.6)
                << QPointF(17.6, 6.6);
        painter.drawPolygon(sparkle);
    }

    return QIcon(pixmap);
}

QIcon trayStateIcon(const QString& resourcePath, TrayGlyphVariant variant)
{
    const QIcon icon(resourcePath);
    if (!icon.isNull()) {
        return icon;
    }
    return fallbackTrayStateIcon(variant);
}

} // namespace

int main(int argc, char* argv[])
{
    // QApplication (not QGuiApplication) is needed for QSystemTrayIcon
    QApplication app(argc, argv);

    // Force Fusion style — native macOS style doesn't support Control customization.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // Force light color scheme — QML uses hardcoded light colors throughout.
    // Without this, Fusion in dark mode renders white text on our light backgrounds.
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    app.setApplicationName(QStringLiteral("BetterSpotlight"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("BetterSpotlight"));
    app.setOrganizationDomain(QStringLiteral("com.betterspotlight"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app_icon_master.png")));

    // Always run from a writable model cache to keep the bundle lean.
    if (!qEnvironmentVariableIsSet("BETTERSPOTLIGHT_MODELS_DIR")) {
        const QString writableModelsDir = bs::ModelRegistry::writableModelsDir();
        QString seedError;
        if (!bs::ModelRegistry::ensureWritableModelsSeeded(&seedError)) {
            qWarning() << "Model bootstrap warning:" << seedError;
        }
        qputenv("BETTERSPOTLIGHT_MODELS_DIR", writableModelsDir.toUtf8());
        qInfo() << "Using writable models dir:" << writableModelsDir;
    }

    // Critical: app runs in background as a status bar app -- no dock icon,
    // no quit when last window closes.
    app.setQuitOnLastWindowClosed(false);

    const QString allowMultiInstanceEnv =
        qEnvironmentVariable("BETTERSPOTLIGHT_ALLOW_MULTI_INSTANCE").trimmed().toLower();
    const bool allowMultiInstance =
        allowMultiInstanceEnv == QLatin1String("1")
        || allowMultiInstanceEnv == QLatin1String("true")
        || allowMultiInstanceEnv == QLatin1String("yes")
        || allowMultiInstanceEnv == QLatin1String("on");

    bs::RuntimeContext runtimeContext;
    QString runtimeError;
    if (!bs::initRuntimeContext(&runtimeContext, &runtimeError)) {
        qCritical() << "Failed to initialize runtime context:" << runtimeError;
        return 1;
    }

    std::unique_ptr<QLockFile> singleInstanceLock;
    if (!allowMultiInstance) {
        singleInstanceLock = std::make_unique<QLockFile>(runtimeContext.lockPath);
        singleInstanceLock->setStaleLockTime(0);
        if (!singleInstanceLock->tryLock(0)) {
            const bool staleRecovered = singleInstanceLock->removeStaleLockFile()
                                        && singleInstanceLock->tryLock(0);
            if (!staleRecovered) {
                qint64 ownerPid = 0;
                QString ownerHost;
                QString ownerApp;
                singleInstanceLock->getLockInfo(&ownerPid, &ownerHost, &ownerApp);
                qWarning() << "Another BetterSpotlight instance is already running"
                           << "(pid:" << ownerPid << "host:" << ownerHost
                           << "app:" << ownerApp << ").";
                QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                        QStringList{
                                            QStringLiteral("-b"),
                                            QStringLiteral("com.betterspotlight.app"),
                                        });
                return 0;
            }
        }
    }

    bs::cleanupOrphanRuntimeDirectories(runtimeContext);

    qInfo() << "BetterSpotlight app starting...";

    // --- Create C++ backend objects ---

    bs::ServiceManager serviceManager;
    bs::HotkeyManager hotkeyManager;
    bs::SearchController searchController;
    bs::OnboardingController onboardingController;
    bs::SettingsController settingsController;
    bs::SystemInteractionCollector systemInteractionCollector;
    bs::UpdateManager updateManager;

    // Wire search health/state through the actorized service manager.
    searchController.setServiceManager(&serviceManager);
    // Legacy wiring retained for compatibility with direct-supervisor fallback paths.
    searchController.setSupervisor(serviceManager.supervisor());
    const auto syncClipboardSignalsFromSettings = [&]() {
        searchController.setClipboardSignalsEnabled(
            settingsController.enableInteractionTracking()
            && settingsController.clipboardSignalEnabled());
    };
    syncClipboardSignalsFromSettings();
    QObject::connect(&settingsController, &bs::SettingsController::clipboardSignalEnabledChanged,
                     &app, syncClipboardSignalsFromSettings);
    QObject::connect(&settingsController, &bs::SettingsController::enableInteractionTrackingChanged,
                     &app, syncClipboardSignalsFromSettings);

    QObject::connect(&systemInteractionCollector,
                     &bs::SystemInteractionCollector::behaviorEventCaptured,
                     &searchController,
                     &bs::SearchController::recordBehaviorEvent);
    QObject::connect(&systemInteractionCollector,
                     &bs::SystemInteractionCollector::collectorHealthChanged,
                     &app,
                     [](const QJsonObject& health) {
        qInfo() << "System interaction collector health:" << health;
    });
    const auto syncBehaviorCollectorFromConsent = [&]() {
        const bool enabled = settingsController.runtimeBoolSetting(
            QStringLiteral("behaviorStreamEnabled"),
            false);
        systemInteractionCollector.setEnabled(enabled);
    };
    QObject::connect(&settingsController, &bs::SettingsController::settingsChanged,
                     &app, [&](const QString& key) {
        if (key.trimmed() == QLatin1String("behaviorStreamEnabled")) {
            syncBehaviorCollectorFromConsent();
        }
    });
    QTimer behaviorCollectorSyncTimer;
    behaviorCollectorSyncTimer.setInterval(5000);
    QObject::connect(&behaviorCollectorSyncTimer, &QTimer::timeout,
                     &app, syncBehaviorCollectorFromConsent);
    behaviorCollectorSyncTimer.start();
    syncBehaviorCollectorFromConsent();

    // Load and keep global hotkey in sync with persisted settings.
    bool hotkeySyncInProgress = false;
    const auto syncHotkeyFromSettings = [&]() {
        if (hotkeySyncInProgress) {
            return;
        }

        hotkeySyncInProgress = true;
        const QString requestedHotkey = settingsController.hotkey();
        if (!hotkeyManager.applyHotkey(requestedHotkey)) {
            const QString activeHotkey = hotkeyManager.hotkey();
            if (!activeHotkey.isEmpty() && activeHotkey != requestedHotkey) {
                settingsController.setHotkey(activeHotkey);
            }
        }
        hotkeySyncInProgress = false;
    };
    syncHotkeyFromSettings();
    QObject::connect(&settingsController, &bs::SettingsController::hotkeyChanged,
                     &app, syncHotkeyFromSettings);

    // Wire settings actions to indexer IPC commands.
    QObject::connect(&settingsController, &bs::SettingsController::indexingPaused,
                     &serviceManager, &bs::ServiceManager::pauseIndexing);
    QObject::connect(&settingsController, &bs::SettingsController::indexingResumed,
                     &serviceManager, &bs::ServiceManager::resumeIndexing);
    QObject::connect(&settingsController, &bs::SettingsController::rebuildIndexRequested,
                     &serviceManager, &bs::ServiceManager::rebuildAll);
    QObject::connect(&settingsController, &bs::SettingsController::rebuildVectorIndexRequested,
                     &serviceManager, &bs::ServiceManager::rebuildVectorIndex);
    QObject::connect(&settingsController, &bs::SettingsController::clearExtractionCacheRequested,
                     &serviceManager, &bs::ServiceManager::clearExtractionCache);
    QObject::connect(&settingsController, &bs::SettingsController::reindexFolderRequested,
                     &serviceManager, &bs::ServiceManager::reindexPath);
    QObject::connect(&settingsController, &bs::SettingsController::checkForUpdatesChanged,
                     &app, [&]() {
        updateManager.setAutomaticallyChecks(settingsController.checkForUpdates());
    });
    updateManager.setAutomaticallyChecks(settingsController.checkForUpdates());
    updateManager.initialize();

    // --- Set up the system tray icon (C++ for reliability on macOS) ---

    QSystemTrayIcon trayIcon;
    const QIcon idleTrayIcon = trayStateIcon(QStringLiteral(":/icons/menubar_idle_v2.png"),
                                             TrayGlyphVariant::Idle);
    const QIcon indexingTrayIconA = trayStateIcon(QStringLiteral(":/icons/menubar_indexing_a_v2.png"),
                                                  TrayGlyphVariant::IndexingA);
    const QIcon indexingTrayIconB = trayStateIcon(QStringLiteral(":/icons/menubar_indexing_b_v2.png"),
                                                  TrayGlyphVariant::IndexingB);
    const QIcon errorTrayIcon = trayStateIcon(QStringLiteral(":/icons/menubar_error_v2.png"),
                                              TrayGlyphVariant::Error);
    trayIcon.setIcon(indexingTrayIconA);
    trayIcon.setToolTip(QStringLiteral("BetterSpotlight - Starting services"));

    QMenu trayMenu;
    QAction* showSearchAction = trayMenu.addAction(QStringLiteral("Show Search"));
    QAction* settingsAction = trayMenu.addAction(QStringLiteral("Settings..."));
    trayMenu.addSeparator();
    QAction* quitAction = trayMenu.addAction(QStringLiteral("Quit BetterSpotlight"));

    trayIcon.setContextMenu(&trayMenu);
    trayIcon.show();

    // --- Set up QML engine ---

    QQmlApplicationEngine engine;

    // Expose C++ objects to QML as context properties
    engine.rootContext()->setContextProperty(QStringLiteral("serviceManagerObj"), &serviceManager);
    engine.rootContext()->setContextProperty(QStringLiteral("hotkeyManagerObj"), &hotkeyManager);
    engine.rootContext()->setContextProperty(QStringLiteral("searchControllerObj"), &searchController);
    engine.rootContext()->setContextProperty(QStringLiteral("onboardingControllerObj"), &onboardingController);
    engine.rootContext()->setContextProperty(QStringLiteral("settingsControllerObj"), &settingsController);
    engine.rootContext()->setContextProperty(QStringLiteral("updateManagerObj"), &updateManager);

    // StatusBarBridge has proper Q_OBJECT signals that QML Connections can bind to
    bs::StatusBarBridge statusBarBridge;
    engine.rootContext()->setContextProperty(QStringLiteral("statusBar"), &statusBarBridge);

    // Load Main.qml from embedded resources
    engine.load(QUrl(QStringLiteral("qrc:/BetterSpotlight/Main.qml")));

    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Failed to load QML";
        return 1;
    }

    // --- Connect tray menu actions ---

    QObject::connect(showSearchAction, &QAction::triggered, [&statusBarBridge]() {
        emit statusBarBridge.showSearchRequested();
    });

    QObject::connect(settingsAction, &QAction::triggered, [&statusBarBridge]() {
        emit statusBarBridge.showSettingsRequested();
    });

    QObject::connect(quitAction, &QAction::triggered, [&]() {
        qInfo() << "Quit requested from tray menu";
        app.quit();
    });

    // Clicking the tray icon opens Index Health for quick diagnostics.
    QObject::connect(&trayIcon, &QSystemTrayIcon::activated,
                     [&statusBarBridge](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick ||
            reason == QSystemTrayIcon::Trigger) {
            emit statusBarBridge.showIndexHealthRequested();
        }
    });

    // --- Connect service status to tray icon state ---

    TrayStateController trayStateController(
        &trayIcon, &serviceManager, idleTrayIcon, indexingTrayIconA, indexingTrayIconB, errorTrayIcon, &app);
    trayStateController.updatePresentation();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]() {
        trayStateController.quiesce();
        systemInteractionCollector.setEnabled(false);
        serviceManager.stop();
    });

    QObject::connect(&serviceManager, &bs::ServiceManager::serviceError,
                     [&](const QString& name, const QString& error) {
        trayIcon.showMessage(QStringLiteral("BetterSpotlight"),
                             QStringLiteral("Service '%1' error: %2").arg(name, error),
                             QSystemTrayIcon::Warning, 5000);
    });

    QObject::connect(&hotkeyManager, &bs::HotkeyManager::hotkeyConflictDetected,
                     [&](const QString& attempted, const QString& error, const QStringList& suggestions) {
        QString message =
            QStringLiteral("Hotkey '%1' is unavailable. %2").arg(attempted, error);
        if (!suggestions.isEmpty()) {
            message += QStringLiteral(" Try: %1").arg(suggestions.join(QStringLiteral(", ")));
        }
        trayIcon.showMessage(QStringLiteral("BetterSpotlight"),
                             message,
                             QSystemTrayIcon::Warning,
                             7000);
    });

    // Gate service start and initial indexing on onboarding completion.
    bool servicesReady = false;
    bool onboardingDone = !onboardingController.needsOnboarding();
    bool servicesStarted = false;
    bool initialIndexingTriggered = false;
    const auto ensureServicesStarted = [&]() {
        if (servicesStarted) {
            return;
        }
        serviceManager.start();
        servicesStarted = true;
    };
    const auto maybeStartInitialIndexing = [&]() {
        if (initialIndexingTriggered || !servicesReady || !onboardingDone) {
            return;
        }
        serviceManager.triggerInitialIndexing();
        initialIndexingTriggered = true;
    };
    QObject::connect(&serviceManager, &bs::ServiceManager::allServicesReady,
                     &app, [&]() {
        servicesReady = true;
        maybeStartInitialIndexing();
    });
    QObject::connect(&onboardingController, &bs::OnboardingController::onboardingCompleted,
                     &app, [&]() {
        onboardingDone = true;
        ensureServicesStarted();
        maybeStartInitialIndexing();
    });

    // --- Start services only when onboarding is already complete ---
    if (onboardingDone) {
        ensureServicesStarted();
    }

    qInfo() << "BetterSpotlight ready";

    return app.exec();
}
