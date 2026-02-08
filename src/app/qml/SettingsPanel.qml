import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs

Window {
    id: settingsWindow

    property var searchController: null
    property var serviceManager: null
    property var settingsController: null

    title: qsTr("BetterSpotlight Settings")
    width: 640
    height: 560
    minimumWidth: 560
    minimumHeight: 480
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
                TabButton { text: qsTr("Privacy") }
                TabButton { text: qsTr("Index Health") }
            }

            // Tab content area
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabBar.currentIndex

                // ==========================================
                // ---- General Tab ----
                // ==========================================
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Global Hotkey")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("Keyboard shortcut to show/hide BetterSpotlight:")
                                    font.pixelSize: 13
                                    color: "#1A1A1A"
                                }

                                HotkeyRecorder {
                                    id: hotkeyRecorder
                                    Layout.fillWidth: true
                                    hotkey: settingsController ? settingsController.hotkey : "Cmd+Space"
                                    onHotkeyChanged: {
                                        if (settingsController) {
                                            settingsController.hotkey = hotkey
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Supported modifiers: Cmd, Ctrl, Alt/Option, Shift")
                                    font.pixelSize: 11
                                    color: "#999999"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Behavior")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 12

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Launch at login")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.launchAtLogin : false
                                        onToggled: {
                                            if (settingsController) {
                                                settingsController.launchAtLogin = checked
                                            }
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Show in Dock")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                        Label {
                                            text: qsTr("Display BetterSpotlight icon in the Dock")
                                            font.pixelSize: 11
                                            color: "#999999"
                                        }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.showInDock : false
                                        onToggled: {
                                            if (settingsController) {
                                                settingsController.showInDock = checked
                                            }
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Check for updates automatically")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.checkForUpdates : true
                                        onToggled: {
                                            if (settingsController) {
                                                settingsController.checkForUpdates = checked
                                            }
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        spacing: 12
                                        Layout.fillWidth: true

                                        Label {
                                            text: qsTr("Maximum results")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: maxResultsSlider.value.toString()
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: "#1A1A1A"
                                        }
                                    }

                                    Slider {
                                        id: maxResultsSlider
                                        Layout.fillWidth: true
                                        from: 5
                                        to: 50
                                        stepSize: 1
                                        value: settingsController ? settingsController.maxResults : 20
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.maxResults = value
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: qsTr("5"); font.pixelSize: 11; color: "#999999" }
                                        Item { Layout.fillWidth: true }
                                        Label { text: qsTr("50"); font.pixelSize: 11; color: "#999999" }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Appearance")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 12

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Theme")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    ComboBox {
                                        id: themeCombo
                                        model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]
                                        property var themeValues: ["system", "light", "dark"]
                                        currentIndex: {
                                            if (!settingsController) return 0
                                            var idx = themeValues.indexOf(settingsController.theme)
                                            return idx >= 0 ? idx : 0
                                        }
                                        onActivated: function(index) {
                                            if (settingsController) settingsController.theme = themeValues[index]
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Language")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    ComboBox {
                                        id: languageCombo
                                        model: [qsTr("English"), qsTr("中文"), qsTr("日本語"), qsTr("한국어"), qsTr("Español"), qsTr("Deutsch"), qsTr("Français")]
                                        property var langValues: ["en", "zh", "ja", "ko", "es", "de", "fr"]
                                        currentIndex: {
                                            if (!settingsController) return 0
                                            var idx = langValues.indexOf(settingsController.language)
                                            return idx >= 0 ? idx : 0
                                        }
                                        onActivated: function(index) {
                                            if (settingsController) settingsController.language = langValues[index]
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ==========================================
                // ---- Indexing Tab ----
                // ==========================================
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Indexed Directories")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("Directories to scan and index for search:")
                                    font.pixelSize: 13
                                    color: "#666666"
                                }

                                DirectoryPicker {
                                    Layout.fillWidth: true
                                    model: settingsController ? settingsController.indexRoots : []
                                    onModelChanged: {
                                        if (settingsController) {
                                            settingsController.indexRoots = model
                                        }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Content Extraction")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 12

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label { text: qsTr("Enable PDF extraction"); font.pixelSize: 13; color: "#1A1A1A" }
                                        Label { text: qsTr("Extract and index text content from PDF files"); font.pixelSize: 11; color: "#999999" }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.enablePdf : true
                                        onToggled: { if (settingsController) settingsController.enablePdf = checked }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label { text: qsTr("Enable OCR extraction"); font.pixelSize: 13; color: "#1A1A1A" }
                                        Label {
                                            text: qsTr("Use optical character recognition for scanned documents and images")
                                            font.pixelSize: 11; color: "#999999"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                        }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.enableOcr : false
                                        onToggled: { if (settingsController) settingsController.enableOcr = checked }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label { text: qsTr("Enable semantic search"); font.pixelSize: 13; color: "#1A1A1A" }
                                        Label {
                                            text: qsTr("Generate vector embeddings for natural language search")
                                            font.pixelSize: 11; color: "#999999"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                        }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.embeddingEnabled : false
                                        onToggled: { if (settingsController) settingsController.embeddingEnabled = checked }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true
                                    visible: settingsController ? settingsController.embeddingEnabled : false

                                    Label { text: qsTr("Embedding model:"); font.pixelSize: 13; color: "#666666" }
                                    Label { text: "BGE-small-en-v1.5"; font.pixelSize: 13; font.family: "Menlo"; color: "#1A1A1A" }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Limits")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label { text: qsTr("Maximum file size"); font.pixelSize: 13; color: "#1A1A1A"; Layout.fillWidth: true }
                                    Label {
                                        text: maxFileSizeSlider.value + " MB"
                                        font.pixelSize: 13; font.weight: Font.DemiBold; color: "#1A1A1A"
                                    }
                                }

                                Slider {
                                    id: maxFileSizeSlider
                                    Layout.fillWidth: true
                                    from: 1; to: 500; stepSize: 1
                                    value: settingsController ? settingsController.maxFileSizeMB : 50
                                    onMoved: { if (settingsController) settingsController.maxFileSizeMB = value }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("1 MB"); font.pixelSize: 11; color: "#999999" }
                                    Item { Layout.fillWidth: true }
                                    Label { text: qsTr("500 MB"); font.pixelSize: 11; color: "#999999" }
                                }
                            }
                        }

                        RowLayout {
                            spacing: 12
                            Layout.fillWidth: true
                            Item { Layout.fillWidth: true }
                            Button {
                                id: pauseIndexingBtn
                                property bool paused: false
                                text: paused ? qsTr("Resume Indexing") : qsTr("Pause Indexing")
                                onClicked: {
                                    if (settingsController) {
                                        if (paused) settingsController.resumeIndexing()
                                        else settingsController.pauseIndexing()
                                        paused = !paused
                                    }
                                }
                            }
                        }
                    }
                }

                // ==========================================
                // ---- Exclusions Tab ----
                // ==========================================
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Exclusion Patterns")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("Files and directories matching these patterns are excluded from indexing.")
                                    font.pixelSize: 13; color: "#666666"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                }

                                PatternEditor {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 260
                                    patterns: settingsController ? settingsController.userPatterns : []
                                    readOnlyPatterns: [
                                        "node_modules/", ".git/", ".svn/", ".hg/",
                                        "build/", "dist/", "__pycache__/", ".tox/",
                                        "*.o", "*.pyc", "*.class",
                                        ".DS_Store", "Thumbs.db",
                                        "Library/Caches/", "Library/Logs/",
                                        ".Trash/", ".Spotlight-V100/",
                                        "*.app/Contents/", "*.framework/"
                                    ]
                                    onPatternsChanged: {
                                        if (settingsController) settingsController.userPatterns = patterns
                                    }
                                }
                            }
                        }

                        RowLayout {
                            spacing: 12
                            Layout.fillWidth: true

                            Button {
                                text: qsTr("Edit .bsignore File")
                                onClicked: {
                                    var homePath = StandardPaths.writableLocation(StandardPaths.HomeLocation)
                                    Qt.openUrlExternally("file://" + homePath + "/.bsignore")
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        GroupBox {
                            id: syntaxHelpBox
                            Layout.fillWidth: true
                            title: qsTr("Syntax Help")
                            property bool expanded: false

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: syntaxHelpBox.expanded ? qsTr("\u25BC Hide syntax reference") : qsTr("\u25B6 Show syntax reference")
                                    font.pixelSize: 13; color: "#666666"
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: syntaxHelpBox.expanded = !syntaxHelpBox.expanded
                                    }
                                }

                                ColumnLayout {
                                    visible: syntaxHelpBox.expanded
                                    spacing: 4
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Pattern syntax follows .gitignore conventions:")
                                        font.pixelSize: 13; color: "#1A1A1A"; font.weight: Font.DemiBold
                                    }

                                    Repeater {
                                        model: [
                                            { pattern: "*", desc: qsTr("matches any sequence of characters") },
                                            { pattern: "?", desc: qsTr("matches any single character") },
                                            { pattern: "**/", desc: qsTr("matches directories at any depth") },
                                            { pattern: "/", desc: qsTr("trailing slash matches directories only") },
                                            { pattern: "!", desc: qsTr("leading ! negates the pattern") },
                                            { pattern: "#", desc: qsTr("lines starting with # are comments") }
                                        ]

                                        delegate: RowLayout {
                                            required property var modelData
                                            spacing: 12
                                            Label { text: modelData.pattern; font.pixelSize: 12; font.family: "Menlo"; color: "#1A1A1A"; Layout.preferredWidth: 40 }
                                            Label { text: modelData.desc; font.pixelSize: 12; color: "#666666" }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ==========================================
                // ---- Privacy Tab ----
                // ==========================================
                ScrollView {
                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Data Collection")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 12

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label { text: qsTr("Enable feedback logging"); font.pixelSize: 13; color: "#1A1A1A" }
                                        Label {
                                            text: qsTr("Log which results you open to improve ranking over time")
                                            font.pixelSize: 11; color: "#999999"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                        }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.enableFeedbackLogging : true
                                        onToggled: { if (settingsController) settingsController.enableFeedbackLogging = checked }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label { text: qsTr("Enable interaction tracking"); font.pixelSize: 13; color: "#1A1A1A" }
                                        Label {
                                            text: qsTr("Track search queries and context signals for better results")
                                            font.pixelSize: 11; color: "#999999"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                        }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.enableInteractionTracking : false
                                        onToggled: { if (settingsController) settingsController.enableInteractionTracking = checked }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label { text: qsTr("Feedback data retention"); font.pixelSize: 13; color: "#1A1A1A"; Layout.fillWidth: true }

                                    ComboBox {
                                        id: retentionCombo
                                        model: [ qsTr("30 days"), qsTr("60 days"), qsTr("90 days"), qsTr("180 days") ]
                                        property var dayValues: [30, 60, 90, 180]
                                        currentIndex: {
                                            if (!settingsController) return 2
                                            var days = settingsController.feedbackRetentionDays
                                            var idx = dayValues.indexOf(days)
                                            return idx >= 0 ? idx : 2
                                        }
                                        onActivated: function(index) {
                                            if (settingsController) settingsController.feedbackRetentionDays = dayValues[index]
                                        }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Sensitive Paths")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("These paths are indexed for metadata only \u2014 file content is never extracted or stored.")
                                    font.pixelSize: 13; color: "#666666"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                }

                                Repeater {
                                    id: sensitiveRepeater
                                    model: settingsController ? settingsController.sensitivePaths : []

                                    delegate: RowLayout {
                                        required property int index
                                        required property string modelData
                                        spacing: 8
                                        Layout.fillWidth: true
                                        Rectangle { width: 6; height: 6; radius: 3; color: "#C62828" }
                                        Label { text: modelData; font.pixelSize: 12; font.family: "Menlo"; color: "#1A1A1A"; Layout.fillWidth: true }
                                        Button {
                                            text: qsTr("Remove")
                                            font.pixelSize: 11
                                            palette.buttonText: "#C62828"
                                            onClicked: {
                                                if (settingsController) {
                                                    var paths = settingsController.sensitivePaths
                                                    paths.splice(index, 1)
                                                    settingsController.sensitivePaths = paths
                                                }
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    spacing: 8
                                    Layout.fillWidth: true

                                    TextField {
                                        id: newSensitivePathField
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("e.g. ~/.config/secrets")
                                        font.pixelSize: 12
                                        font.family: "Menlo"
                                    }
                                    Button {
                                        text: qsTr("Add")
                                        enabled: newSensitivePathField.text.trim().length > 0
                                        onClicked: {
                                            if (settingsController) {
                                                var paths = settingsController.sensitivePaths
                                                var newPath = newSensitivePathField.text.trim()
                                                if (paths.indexOf(newPath) === -1) {
                                                    paths.push(newPath)
                                                    settingsController.sensitivePaths = paths
                                                }
                                                newSensitivePathField.text = ""
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Data Management")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 12

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Button {
                                        text: qsTr("Clear All Feedback Data")
                                        palette.buttonText: "#C62828"
                                        onClicked: clearFeedbackDialog.open()
                                    }

                                    Item { Layout.fillWidth: true }

                                    Button {
                                        text: qsTr("Export My Data")
                                        onClicked: {
                                            if (settingsController) {
                                                settingsController.exportData()
                                                exportConfirmLabel.visible = true
                                                exportConfirmTimer.start()
                                            }
                                        }
                                    }
                                }

                                Label {
                                    id: exportConfirmLabel
                                    visible: false
                                    text: qsTr("Data exported to ~/Downloads/betterspotlight-data-export.json")
                                    font.pixelSize: 11; color: "#2E7D32"
                                    Timer { id: exportConfirmTimer; interval: 5000; onTriggered: exportConfirmLabel.visible = false }
                                }
                            }
                        }
                    }
                }

                // ==========================================
                // ---- Index Health Tab ----
                // ==========================================
                ScrollView {
                    id: healthTab

                    property var healthData: ({})
                    property bool loaded: false
                    property bool vectorRebuildRunning: {
                        if (!healthData) return false
                        return (healthData["vectorRebuildStatus"] || "idle") === "running"
                    }

                    function refreshHealth() {
                        if (!searchController) {
                            return
                        }

                        var next = searchController.getHealthSync()
                        if (next && Object.keys(next).length > 0) {
                            healthTab.healthData = next
                            healthTab.loaded = true
                        } else if (!healthTab.loaded) {
                            healthTab.healthData = ({})
                        }

                        if (healthTab.vectorRebuildRunning) {
                            if (!healthRefreshTimer.running) {
                                healthRefreshTimer.start()
                            }
                        } else {
                            healthRefreshTimer.stop()
                        }
                    }

                    Timer {
                        id: healthRefreshTimer
                        interval: 2000
                        repeat: true
                        running: false
                        onTriggered: healthTab.refreshHealth()
                    }

                    Component.onCompleted: healthTab.refreshHealth()

                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        // Status indicator banner
                        Rectangle {
                            Layout.fillWidth: true
                            height: 48; radius: 6
                            color: {
                                if (!healthTab.loaded) return "#F0F0F0"
                                var s = healthTab.healthData["overallStatus"]
                                if (s === "healthy") return "#E8F5E9"
                                if (s === "degraded") return "#FFF8E1"
                                if (s === "rebuilding") return "#E3F2FD"
                                return "#FFEBEE"
                            }
                            border.width: 1
                            border.color: {
                                if (!healthTab.loaded) return "#C0C0C0"
                                var s = healthTab.healthData["overallStatus"]
                                if (s === "healthy") return "#2E7D32"
                                if (s === "degraded") return "#F57F17"
                                if (s === "rebuilding") return "#1565C0"
                                return "#C62828"
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 16; anchors.rightMargin: 16
                                spacing: 12

                                Rectangle {
                                    width: 12; height: 12; radius: 6
                                    color: {
                                        if (!healthTab.loaded) return "#999999"
                                        var s = healthTab.healthData["overallStatus"]
                                        if (s === "healthy") return "#2E7D32"
                                        if (s === "degraded") return "#F57F17"
                                        if (s === "rebuilding") return "#1565C0"
                                        return "#C62828"
                                    }
                                }

                                Label {
                                    text: {
                                        if (!healthTab.loaded) return qsTr("Not loaded \u2014 click Refresh")
                                        var s = healthTab.healthData["overallStatus"]
                                        if (s === "healthy") return qsTr("Index Healthy")
                                        if (s === "degraded") return qsTr("Index Degraded")
                                        if (s === "rebuilding") return qsTr("Index Rebuilding")
                                        return qsTr("Index Error")
                                    }
                                    font.pixelSize: 14; font.weight: Font.DemiBold; color: "#1A1A1A"
                                    Layout.fillWidth: true
                                }

                                Button {
                                    text: qsTr("Refresh")
                                    onClicked: healthTab.refreshHealth()
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Index Statistics")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Indexed Files"), key: "totalIndexedItems", format: "int" },
                                        { label: qsTr("Content Chunks"), key: "totalChunks", format: "int" },
                                        { label: qsTr("Embedded Vectors"), key: "totalEmbeddedVectors", format: "int" },
                                        { label: qsTr("Content Coverage"), key: "contentCoveragePct", format: "percent" },
                                        { label: qsTr("Semantic Coverage"), key: "semanticCoveragePct", format: "percent" },
                                        { label: qsTr("Database Size"), key: "ftsIndexSize", format: "bytes" },
                                        { label: qsTr("Vector Index Size"), key: "vectorIndexSize", format: "bytes" },
                                        { label: qsTr("Last Full Scan"), key: "lastScanTime", format: "timestamp" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12; Layout.fillWidth: true
                                        Label { text: modelData.label + ":"; font.weight: Font.DemiBold; font.pixelSize: 13; color: "#1A1A1A"; Layout.preferredWidth: 160 }
                                        Label { text: formatHealthValue(healthTab.healthData, modelData.key, modelData.format); font.pixelSize: 13; color: "#1A1A1A" }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Index Roots")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 6

                                Repeater {
                                    model: {
                                        if (!healthTab.loaded) return []
                                        return healthTab.healthData["indexRoots"] || []
                                    }

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 8; Layout.fillWidth: true

                                        Rectangle {
                                            width: 8; height: 8; radius: 4
                                            color: {
                                                var st = modelData.status || "unknown"
                                                if (st === "active") return "#2E7D32"
                                                if (st === "scanning") return "#F57F17"
                                                if (st === "error") return "#C62828"
                                                return "#999999"
                                            }
                                        }
                                        Label {
                                            text: modelData.path || modelData
                                            font.pixelSize: 12; font.family: "Menlo"; color: "#1A1A1A"
                                            Layout.fillWidth: true; elide: Text.ElideMiddle
                                        }
                                        Label {
                                            text: {
                                                var st = modelData.status || "unknown"
                                                if (st === "active") return qsTr("Active")
                                                if (st === "scanning") return qsTr("Scanning")
                                                if (st === "error") return qsTr("Error")
                                                return qsTr("Unknown")
                                            }
                                            font.pixelSize: 11; color: "#666666"
                                        }
                                    }
                                }

                                Label {
                                    visible: !healthTab.loaded
                                    text: qsTr("Click Refresh to load index root status.")
                                    font.pixelSize: 11; color: "#999999"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Queue")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Pending"), key: "queuePending", format: "int" },
                                        { label: qsTr("In Progress"), key: "queueInProgress", format: "int" },
                                        { label: qsTr("Dropped"), key: "queueDropped", format: "int" },
                                        { label: qsTr("Embedding Queue"), key: "queueEmbedding", format: "int" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12; Layout.fillWidth: true
                                        Label { text: modelData.label + ":"; font.weight: Font.DemiBold; font.pixelSize: 13; color: "#1A1A1A"; Layout.preferredWidth: 160 }
                                        Label { text: formatHealthValue(healthTab.healthData, modelData.key, modelData.format); font.pixelSize: 13; color: "#1A1A1A" }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Vector Rebuild")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Status"), key: "vectorRebuildStatus", format: "status" },
                                        { label: qsTr("Progress"), key: "vectorRebuildProgressPct", format: "percent" },
                                        { label: qsTr("Run ID"), key: "vectorRebuildRunId", format: "int" },
                                        { label: qsTr("Processed"), key: "vectorRebuildProcessed", format: "int" },
                                        { label: qsTr("Candidates"), key: "vectorRebuildTotalCandidates", format: "int" },
                                        { label: qsTr("Scope Candidates"), key: "vectorRebuildScopeCandidates", format: "int" },
                                        { label: qsTr("Embedded"), key: "vectorRebuildEmbedded", format: "int" },
                                        { label: qsTr("Skipped"), key: "vectorRebuildSkipped", format: "int" },
                                        { label: qsTr("Failed"), key: "vectorRebuildFailed", format: "int" },
                                        { label: qsTr("Started"), key: "vectorRebuildStartedAt", format: "timestamp" },
                                        { label: qsTr("Finished"), key: "vectorRebuildFinishedAt", format: "timestamp" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12; Layout.fillWidth: true
                                        Label { text: modelData.label + ":"; font.weight: Font.DemiBold; font.pixelSize: 13; color: "#1A1A1A"; Layout.preferredWidth: 160 }
                                        Label { text: formatHealthValue(healthTab.healthData, modelData.key, modelData.format); font.pixelSize: 13; color: "#1A1A1A" }
                                    }
                                }

                                Label {
                                    visible: !!(healthTab.healthData && healthTab.healthData["vectorRebuildLastError"])
                                    text: qsTr("Last Error: ") + (healthTab.healthData["vectorRebuildLastError"] || "")
                                    font.pixelSize: 11
                                    color: "#C62828"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: qsTr("Scope Roots: ") + ((healthTab.healthData["vectorRebuildScopeRoots"] || []).join(", "))
                                    visible: !!(healthTab.healthData
                                                && healthTab.healthData["vectorRebuildScopeRoots"]
                                                && healthTab.healthData["vectorRebuildScopeRoots"].length > 0)
                                    font.pixelSize: 11
                                    color: "#666666"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Recent Errors")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 4

                                Repeater {
                                    model: {
                                        if (!healthTab.loaded) return []
                                        return healthTab.healthData["recentErrors"] || []
                                    }

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 8; Layout.fillWidth: true
                                        Label { text: modelData.path || ""; font.pixelSize: 11; font.family: "Menlo"; color: "#C62828"; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                        Label { text: modelData.error || ""; font.pixelSize: 11; color: "#666666"; Layout.preferredWidth: 200; elide: Text.ElideRight }
                                    }
                                }

                                Label {
                                    visible: {
                                        if (!healthTab.loaded) return true
                                        var errors = healthTab.healthData["recentErrors"]
                                        return !errors || errors.length === 0
                                    }
                                    text: healthTab.loaded ? qsTr("No recent errors.") : qsTr("Click Refresh to load error data.")
                                    font.pixelSize: 11; color: "#999999"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Actions")

                            RowLayout {
                                anchors.fill: parent
                                spacing: 12

                                Button { text: qsTr("Reindex Folder\u2026"); onClicked: reindexFolderDialog.open() }
                                Button { text: qsTr("Rebuild All"); onClicked: rebuildAllDialog.open() }
                                Button {
                                    text: qsTr("Rebuild Vector Index")
                                    enabled: settingsController
                                             ? (settingsController.embeddingEnabled
                                                && !healthTab.vectorRebuildRunning)
                                             : false
                                    onClicked: rebuildVectorDialog.open()
                                }
                                Button { text: qsTr("Clear Cache"); onClicked: clearCacheDialog.open() }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Dialogs ----

    MessageDialog {
        id: clearFeedbackDialog
        title: qsTr("Clear Feedback Data")
        text: qsTr("Are you sure you want to clear all feedback data? This will remove all search history and ranking adjustments. This action cannot be undone.")
        buttons: MessageDialog.Ok | MessageDialog.Cancel
        onAccepted: { if (settingsController) settingsController.clearFeedbackData() }
    }

    MessageDialog {
        id: rebuildAllDialog
        title: qsTr("Rebuild Index")
        text: qsTr("Are you sure you want to rebuild the entire index? This may take a while depending on the number of files.")
        buttons: MessageDialog.Ok | MessageDialog.Cancel
        onAccepted: { if (settingsController) settingsController.rebuildIndex() }
    }

    MessageDialog {
        id: rebuildVectorDialog
        title: qsTr("Rebuild Vector Index")
        text: qsTr("Are you sure you want to rebuild the vector index? All embeddings will be regenerated.")
        buttons: MessageDialog.Ok | MessageDialog.Cancel
        onAccepted: {
            if (settingsController) {
                settingsController.rebuildVectorIndex()
                healthTab.refreshHealth()
            }
        }
    }

    MessageDialog {
        id: clearCacheDialog
        title: qsTr("Clear Cache")
        text: qsTr("Are you sure you want to clear the extraction cache? Cached content will need to be re-extracted on next scan.")
        buttons: MessageDialog.Ok | MessageDialog.Cancel
        onAccepted: { if (settingsController) settingsController.clearExtractionCache() }
    }

    FolderDialog {
        id: reindexFolderDialog
        title: qsTr("Select Folder to Reindex")
        onAccepted: { if (settingsController) settingsController.reindexFolder(selectedFolder.toString()) }
    }

    // ---- Helper functions ----

    function statusColor(status: string): string {
        switch (status) {
        case "running": return "#2E7D32"
        case "starting": return "#F57F17"
        case "stopped": return "#999999"
        case "crashed": return "#C62828"
        case "error": return "#C62828"
        default: return "#999999"
        }
    }

    function formatHealthValue(data, key: string, format: string): string {
        if (!data || data[key] === undefined) return "--"
        var val = data[key]
        switch (format) {
        case "bool":
            return val ? qsTr("Healthy") : qsTr("Unhealthy")
        case "status":
            if (!val || val === "idle") return qsTr("Idle")
            if (val === "running") return qsTr("Running")
            if (val === "succeeded") return qsTr("Succeeded")
            if (val === "failed") return qsTr("Failed")
            return String(val)
        case "int":
            return Number(val).toLocaleString()
        case "percent":
            return Number(val).toFixed(1) + "%"
        case "bytes":
            var bytes = Number(val)
            if (bytes < 1024) return bytes + " B"
            if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
            if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
            return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
        case "timestamp":
            if (!val || val === "") return "--"
            var d = new Date(val)
            return Qt.formatDateTime(d, "yyyy-MM-dd hh:mm")
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
