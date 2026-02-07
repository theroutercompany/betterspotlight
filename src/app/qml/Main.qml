import QtQuick
import QtQuick.Controls

// Root is a non-visual object — BetterSpotlight is a status-bar app
// with no main window. Using QtObject avoids the macOS issue where
// child Windows of an invisible ApplicationWindow can't be shown.
QtObject {
    id: root

    // The search panel is a standalone floating window
    property var searchPanel: SearchPanel {
        searchController: searchControllerObj
    }

    // The settings panel is a standalone window
    property var settingsPanel: SettingsPanel {
        searchController: searchControllerObj
        serviceManager: serviceManagerObj
    }

    // Connect the global hotkey to toggle the search panel
    property var hotkeyConnection: Connections {
        target: hotkeyManagerObj

        function onHotkeyTriggered() {
            if (searchPanel.visible) {
                searchPanel.dismiss()
            } else {
                searchPanel.showAndActivate()
            }
        }
    }

    // Connect to the status bar bridge (tray menu actions)
    property var statusBarConnection: Connections {
        target: statusBar

        function onShowSearchRequested() {
            searchPanel.showAndActivate()
        }

        function onShowSettingsRequested() {
            settingsPanel.show()
            settingsPanel.raise()
            settingsPanel.requestActivate()
        }
    }

    // Monitor service readiness
    property var serviceConnection: Connections {
        target: serviceManagerObj

        function onAllServicesReady() {
            console.log("All services are ready")
        }

        function onServiceError(serviceName, error) {
            console.warn("Service error:", serviceName, "-", error)
        }
    }
}
