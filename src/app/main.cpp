#include "hotkey_manager.h"
#include "search_controller.h"
#include "service_manager.h"
#include "status_bar_bridge.h"
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

    // Wire the search controller to the supervisor for IPC
    searchController.setSupervisor(serviceManager.supervisor());

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

    // --- Register the global hotkey ---

    if (!hotkeyManager.registerHotkey()) {
        qWarning() << "Failed to register global hotkey. "
                       "The hotkey may be in use by another application.";
    }

    // --- Start services ---

    serviceManager.start();

    qInfo() << "BetterSpotlight ready";

    return app.exec();
}
