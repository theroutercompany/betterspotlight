import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: settingsWindow

    property var searchController: null
    property var serviceManager: null

    title: qsTr("BetterSpotlight Settings")
    width: 560
    height: 480
    minimumWidth: 480
    minimumHeight: 400
    visible: false

    // Center on screen
    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    Rectangle {
        anchors.fill: parent
        color: "#F6F6F6"

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            // Tab bar
            TabBar {
                id: tabBar
                Layout.fillWidth: true

                TabButton { text: qsTr("General") }
                TabButton { text: qsTr("Indexing") }
                TabButton { text: qsTr("Exclusions") }
                TabButton { text: qsTr("Index Health") }
            }

            // Tab content area
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabBar.currentIndex

                // ---- General Tab ----
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Hotkey")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("Global hotkey to show/hide the search panel:")
                                    font.pixelSize: 13
                                }

                                RowLayout {
                                    spacing: 8

                                    TextField {
                                        id: hotkeyField
                                        Layout.preferredWidth: 200
                                        text: hotkeyManagerObj ? hotkeyManagerObj.hotkey : "Cmd+Space"
                                        placeholderText: qsTr("e.g. Cmd+Space")
                                        font.pixelSize: 13
                                    }

                                    Button {
                                        text: qsTr("Apply")
                                        onClicked: {
                                            if (hotkeyManagerObj) {
                                                hotkeyManagerObj.hotkey = hotkeyField.text
                                            }
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Supported modifiers: Cmd, Ctrl, Alt/Option, Shift")
                                    font.pixelSize: 11
                                    color: "#888888"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Appearance")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("BetterSpotlight runs as a status bar app.\nUse the hotkey or status bar icon to access search.")
                                    font.pixelSize: 13
                                    color: "#666666"
                                }
                            }
                        }
                    }
                }

                // ---- Indexing Tab ----
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Indexed Paths")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("Directories being indexed for search:")
                                    font.pixelSize: 13
                                }

                                // Hardcoded default for now; will be wired to Settings
                                Repeater {
                                    model: ["/Users", "/Applications", "/opt"]

                                    delegate: RowLayout {
                                        spacing: 8

                                        Label {
                                            text: modelData
                                            font.pixelSize: 13
                                            font.family: "Menlo"
                                            Layout.fillWidth: true
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("(Path configuration will be available in a future release.)")
                                    font.pixelSize: 11
                                    color: "#999999"
                                    Layout.topMargin: 8
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Service Status")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                RowLayout {
                                    spacing: 8
                                    Label { text: qsTr("Indexer:"); font.weight: Font.DemiBold; font.pixelSize: 13 }
                                    Label {
                                        text: serviceManager ? serviceManager.indexerStatus : "unknown"
                                        font.pixelSize: 13
                                        color: statusColor(text)
                                    }
                                }

                                RowLayout {
                                    spacing: 8
                                    Label { text: qsTr("Extractor:"); font.weight: Font.DemiBold; font.pixelSize: 13 }
                                    Label {
                                        text: serviceManager ? serviceManager.extractorStatus : "unknown"
                                        font.pixelSize: 13
                                        color: statusColor(text)
                                    }
                                }

                                RowLayout {
                                    spacing: 8
                                    Label { text: qsTr("Query:"); font.weight: Font.DemiBold; font.pixelSize: 13 }
                                    Label {
                                        text: serviceManager ? serviceManager.queryStatus : "unknown"
                                        font.pixelSize: 13
                                        color: statusColor(text)
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- Exclusions Tab ----
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Default Exclusion Patterns")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 4

                                Label {
                                    text: qsTr("Files and directories matching these patterns are excluded from indexing.\nCustom patterns can be added to ~/.bsignore")
                                    font.pixelSize: 13
                                    color: "#666666"
                                    Layout.bottomMargin: 8
                                }

                                Repeater {
                                    model: [
                                        "node_modules/", ".git/", ".svn/",
                                        ".hg/", "build/", "dist/",
                                        "__pycache__/", ".tox/",
                                        "*.o", "*.pyc", "*.class",
                                        ".DS_Store", "Thumbs.db",
                                        "Library/Caches/", "Library/Logs/",
                                        ".Trash/", ".Spotlight-V100/",
                                        "*.app/Contents/", "*.framework/"
                                    ]

                                    delegate: Label {
                                        text: modelData
                                        font.pixelSize: 12
                                        font.family: "Menlo"
                                        color: "#444444"
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- Index Health Tab ----
                ScrollView {
                    id: healthTab

                    property var healthData: ({})
                    property bool loaded: false

                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Index Statistics")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Status"), key: "isHealthy", format: "bool" },
                                        { label: qsTr("Total Indexed Items"), key: "totalIndexedItems", format: "int" },
                                        { label: qsTr("Total Chunks"), key: "totalChunks", format: "int" },
                                        { label: qsTr("Items Without Content"), key: "itemsWithoutContent", format: "int" },
                                        { label: qsTr("Total Failures"), key: "totalFailures", format: "int" },
                                        { label: qsTr("FTS Index Size"), key: "ftsIndexSize", format: "bytes" },
                                        { label: qsTr("Index Age"), key: "indexAge", format: "duration" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12

                                        Label {
                                            text: modelData.label + ":"
                                            font.weight: Font.DemiBold
                                            font.pixelSize: 13
                                            Layout.preferredWidth: 180
                                        }

                                        Label {
                                            text: formatHealthValue(healthTab.healthData, modelData.key, modelData.format)
                                            font.pixelSize: 13
                                            color: (modelData.key === "isHealthy")
                                                   ? (healthTab.healthData[modelData.key] ? "#2E7D32" : "#C62828")
                                                   : "#333333"
                                        }
                                    }
                                }

                                Button {
                                    text: qsTr("Refresh")
                                    Layout.topMargin: 12
                                    onClicked: {
                                        if (searchController) {
                                            healthTab.healthData = searchController.getHealthSync()
                                            healthTab.loaded = true
                                        }
                                    }
                                }

                                Label {
                                    visible: !healthTab.loaded
                                    text: qsTr("Click 'Refresh' to load index health data.")
                                    font.pixelSize: 11
                                    color: "#999999"
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    function statusColor(status: string): string {
        switch (status) {
        case "running": return "#2E7D32"
        case "starting": return "#F57F17"
        case "stopped": return "#888888"
        case "crashed": return "#C62828"
        case "error": return "#C62828"
        default: return "#888888"
        }
    }

    function formatHealthValue(data, key: string, format: string): string {
        if (!data || data[key] === undefined) {
            return "--"
        }

        var val = data[key]

        switch (format) {
        case "bool":
            return val ? qsTr("Healthy") : qsTr("Unhealthy")
        case "int":
            return Number(val).toLocaleString()
        case "bytes":
            var bytes = Number(val)
            if (bytes < 1024) return bytes + " B"
            if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
            if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
            return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
        case "duration":
            var secs = Number(val)
            if (secs < 60) return Math.round(secs) + "s"
            if (secs < 3600) return Math.round(secs / 60) + "m"
            if (secs < 86400) return (secs / 3600).toFixed(1) + "h"
            return (secs / 86400).toFixed(1) + "d"
        default:
            return String(val)
        }
    }
}
