import QtQuick

// StatusBar.qml
//
// This is a minimal QML placeholder for the system tray integration.
// On macOS, QSystemTrayIcon from C++ is more reliable than the QML
// SystemTrayIcon (which requires Qt.labs.platform and can be flaky).
//
// The actual system tray icon is created and managed in main.cpp using
// QSystemTrayIcon. This QML file exists as part of the QML module for
// potential future use (e.g., custom tray popover or status overlay).

QtObject {
    id: statusBarRoot

    // Signals that the C++ StatusBar bridges from QSystemTrayIcon actions
    signal showSearchRequested()
    signal showSettingsRequested()
    signal showIndexHealthRequested()

    // Service state for icon appearance (idle, indexing, error)
    property string state: "idle"  // "idle", "indexing", "error"
}
