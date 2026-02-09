#include "hotkey_manager.h"
#include "onboarding_controller.h"
#include "search_controller.h"
#include "service_manager.h"
#include "settings_controller.h"
#include "status_bar_bridge.h"
#include "update_manager.h"
#include "core/shared/logging.h"

#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QtGui/QStyleHints>
#include <QSystemTrayIcon>

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

    // Critical: app runs in background as a status bar app -- no dock icon,
    // no quit when last window closes.
    app.setQuitOnLastWindowClosed(false);

    qInfo() << "BetterSpotlight app starting...";

    // --- Create C++ backend objects ---

    bs::ServiceManager serviceManager;
    bs::HotkeyManager hotkeyManager;
    bs::SearchController searchController;
    bs::OnboardingController onboardingController;
    bs::SettingsController settingsController;
    bs::UpdateManager updateManager;

    // Wire the search controller to the supervisor for IPC
    searchController.setSupervisor(serviceManager.supervisor());

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
    trayIcon.setIcon(QIcon::fromTheme(QStringLiteral("edit-find"),
                                       QIcon(QStringLiteral(":/icons/tray-icon.png"))));
    trayIcon.setToolTip(QStringLiteral("BetterSpotlight"));

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
        serviceManager.stop();
        app.quit();
    });

    // Also support double-click on tray icon to show search
    QObject::connect(&trayIcon, &QSystemTrayIcon::activated,
                     [&statusBarBridge](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick ||
            reason == QSystemTrayIcon::Trigger) {
            emit statusBarBridge.showSearchRequested();
        }
    });

    // --- Connect service status to tray icon state ---

    QObject::connect(&serviceManager, &bs::ServiceManager::serviceStatusChanged, [&]() {
        if (serviceManager.isReady()) {
            trayIcon.setToolTip(QStringLiteral("BetterSpotlight - Ready"));
        } else {
            QString tip = QStringLiteral("BetterSpotlight - Indexer: %1, Query: %2")
                              .arg(serviceManager.indexerStatus(), serviceManager.queryStatus());
            trayIcon.setToolTip(tip);
        }
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

    // --- Start services ---

    serviceManager.start();

    qInfo() << "BetterSpotlight ready";

    return app.exec();
}
