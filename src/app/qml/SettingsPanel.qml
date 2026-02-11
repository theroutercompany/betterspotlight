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
    property string hotkeyConflictMessage: ""
    property var hotkeySuggestions: []

    title: qsTr("BetterSpotlight Settings")
    width: 640
    height: 560
    minimumWidth: 560
    minimumHeight: 480
    visible: false

    // Center on screen
    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    function openIndexHealth() {
        tabBar.currentIndex = 4
        show()
        raise()
        requestActivate()
        healthTab.refreshHealth()
    }

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
                                        if (settingsController && hotkeyManagerObj) {
                                            if (hotkeyManagerObj.applyHotkey(hotkey)) {
                                                settingsController.hotkey = hotkey
                                                settingsWindow.hotkeyConflictMessage = ""
                                                settingsWindow.hotkeySuggestions = []
                                            } else {
                                                hotkeyRecorder.hotkey = settingsController.hotkey
                                                settingsWindow.hotkeyConflictMessage =
                                                    hotkeyManagerObj.registrationError || qsTr("Hotkey is unavailable.")
                                                settingsWindow.hotkeySuggestions =
                                                    hotkeyManagerObj.suggestedAlternatives || []
                                            }
                                        } else if (settingsController) {
                                            settingsController.hotkey = hotkey
                                        }
                                    }
                                }

                                Label {
                                    visible: settingsWindow.hotkeyConflictMessage.length > 0
                                    text: settingsWindow.hotkeyConflictMessage
                                    font.pixelSize: 11
                                    color: "#C62828"
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }

                                Flow {
                                    visible: settingsWindow.hotkeySuggestions.length > 0
                                    spacing: 6
                                    Layout.fillWidth: true

                                    Repeater {
                                        model: settingsWindow.hotkeySuggestions
                                        delegate: Button {
                                            required property var modelData
                                            text: modelData
                                            onClicked: {
                                                if (!settingsController || !hotkeyManagerObj) return
                                                var candidate = String(modelData)
                                                if (hotkeyManagerObj.applyHotkey(candidate)) {
                                                    settingsController.hotkey = candidate
                                                    hotkeyRecorder.hotkey = candidate
                                                    settingsWindow.hotkeyConflictMessage = ""
                                                    settingsWindow.hotkeySuggestions = []
                                                }
                                            }
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

                                Label {
                                    Layout.fillWidth: true
                                    visible: settingsController
                                             && settingsController.platformStatusMessage
                                             && settingsController.platformStatusMessage.length > 0
                                    text: settingsController ? settingsController.platformStatusMessage : ""
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                    color: settingsController && settingsController.platformStatusSuccess
                                           ? "#2E7D32"
                                           : "#C62828"
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

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Update status: ")
                                              + (updateManagerObj ? (updateManagerObj.lastStatus || "idle") : "unavailable")
                                        font.pixelSize: 11
                                        color: "#666666"
                                        Layout.fillWidth: true
                                    }

                                    Button {
                                        text: qsTr("Check Now")
                                        enabled: updateManagerObj && updateManagerObj.available
                                        onClicked: {
                                            if (updateManagerObj) {
                                                updateManagerObj.checkNow()
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

                                    ColumnLayout {
                                        spacing: 2
                                        Layout.fillWidth: true
                                        Label { text: qsTr("Clipboard path signals"); font.pixelSize: 13; color: "#1A1A1A" }
                                        Label {
                                            text: qsTr("Boost results using path-like clipboard hints only. Clipboard text is never persisted.")
                                            font.pixelSize: 11; color: "#999999"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                        }
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.clipboardSignalEnabled : false
                                        enabled: settingsController ? settingsController.enableInteractionTracking : false
                                        onToggled: { if (settingsController) settingsController.clipboardSignalEnabled = checked }
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
                    property var diagnosticsData: []
                    property bool loaded: false
                    property string actionStatusMessage: ""
                    property bool actionStatusIsError: false
                    property bool vectorRebuildRunning: {
                        if (!healthData) return false
                        return (healthData["vectorRebuildStatus"] || "idle") === "running"
                    }

                    function setActionStatus(message, isError) {
                        actionStatusMessage = message || ""
                        actionStatusIsError = !!isError
                        if (actionStatusMessage.length > 0) {
                            actionStatusTimer.restart()
                        } else {
                            actionStatusTimer.stop()
                        }
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
                        if (serviceManager && serviceManager.serviceDiagnostics) {
                            healthTab.diagnosticsData = serviceManager.serviceDiagnostics()
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

                    Timer {
                        id: actionStatusTimer
                        interval: 5000
                        repeat: false
                        onTriggered: healthTab.actionStatusMessage = ""
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
                            title: qsTr("Supervisor")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 6

                                Repeater {
                                    model: healthTab.diagnosticsData || []
                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 10
                                        Layout.fillWidth: true

                                        Label {
                                            text: modelData.name || ""
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            color: "#1A1A1A"
                                            Layout.preferredWidth: 90
                                        }
                                        Label {
                                            text: (modelData.running ? qsTr("running") : qsTr("stopped"))
                                                  + (modelData.ready ? qsTr(" / ready") : qsTr(" / not-ready"))
                                            font.pixelSize: 11
                                            color: modelData.running ? "#2E7D32" : "#999999"
                                            Layout.preferredWidth: 130
                                        }
                                        Label {
                                            text: qsTr("Crashes: %1").arg(Number(modelData.crashCount || 0).toLocaleString())
                                            font.pixelSize: 11
                                            color: Number(modelData.crashCount || 0) > 0 ? "#C62828" : "#666666"
                                            Layout.preferredWidth: 90
                                        }
                                        Label {
                                            text: qsTr("PID: %1").arg(Number(modelData.pid || 0))
                                            font.pixelSize: 11
                                            color: "#666666"
                                            Layout.fillWidth: true
                                        }
                                    }
                                }

                                Label {
                                    visible: !healthTab.diagnosticsData || healthTab.diagnosticsData.length === 0
                                    text: qsTr("No supervisor diagnostics available.")
                                    font.pixelSize: 11
                                    color: "#999999"
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
                            title: qsTr("Process Metrics")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Query"), key: "query" },
                                        { label: qsTr("Indexer"), key: "indexer" },
                                        { label: qsTr("Extractor"), key: "extractor" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12
                                        Layout.fillWidth: true

                                        Label {
                                            text: modelData.label + ":"
                                            font.weight: Font.DemiBold
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.preferredWidth: 110
                                        }
                                        Label {
                                            property var processStats: (healthTab.healthData["processStats"] || {})
                                            property var serviceStats: (processStats[modelData.key] || {})
                                            text: {
                                                if (!serviceStats || !serviceStats["available"]) {
                                                    return qsTr("Unavailable")
                                                }
                                                var pid = serviceStats["pid"] || 0
                                                var rss = Number(serviceStats["rssBytes"] || 0)
                                                var cpu = Number(serviceStats["cpuPercent"] || 0)
                                                var rssMb = (rss / (1024 * 1024)).toFixed(1)
                                                return qsTr("PID %1  RSS %2 MB  CPU %3%")
                                                    .arg(pid)
                                                    .arg(rssMb)
                                                    .arg(cpu.toFixed(1))
                                            }
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Exclusions")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 6

                                Label {
                                    property var bsignore: (healthTab.healthData["bsignoreDetails"] || {})
                                    text: qsTr("Path: ") + (bsignore["path"] || healthTab.healthData["bsignorePath"] || "--")
                                    font.pixelSize: 12
                                    color: "#1A1A1A"
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }

                                Label {
                                    property var bsignore: (healthTab.healthData["bsignoreDetails"] || {})
                                    text: qsTr("Loaded: %1   Patterns: %2")
                                        .arg((bsignore["loaded"] === true || healthTab.healthData["bsignoreLoaded"] === true)
                                             ? qsTr("Yes") : qsTr("No"))
                                        .arg(Number(bsignore["patternCount"] || healthTab.healthData["bsignorePatternCount"] || 0).toLocaleString())
                                    font.pixelSize: 12
                                    color: "#1A1A1A"
                                }

                                Label {
                                    property var bsignore: (healthTab.healthData["bsignoreDetails"] || {})
                                    text: qsTr("Last Reloaded: ")
                                          + formatHealthValue(
                                              {value: bsignore["lastLoadedAt"] || healthTab.healthData["bsignoreLastLoadedAtMs"] || 0},
                                              "value",
                                              "timestamp")
                                    font.pixelSize: 12
                                    color: "#666666"
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
                                        var detailed = healthTab.healthData["detailedFailures"] || []
                                        if (detailed.length > 0) return detailed
                                        return healthTab.healthData["recentErrors"] || []
                                    }

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 8; Layout.fillWidth: true

                                        Rectangle {
                                            visible: !!modelData.severity
                                            radius: 4
                                            color: modelData.severity === "critical" ? "#FFEBEE" : "#FFF8E1"
                                            border.color: modelData.severity === "critical" ? "#C62828" : "#F57F17"
                                            border.width: 1
                                            Layout.preferredHeight: 18
                                            Layout.preferredWidth: severityLabel.implicitWidth + 10

                                            Label {
                                                id: severityLabel
                                                anchors.centerIn: parent
                                                text: modelData.severity || ""
                                                font.pixelSize: 9
                                                color: modelData.severity === "critical" ? "#B71C1C" : "#8A4B00"
                                            }
                                        }

                                        Label {
                                            text: modelData.path || ""
                                            font.pixelSize: 11
                                            font.family: "Menlo"
                                            color: modelData.severity === "critical" ? "#C62828" : "#8A4B00"
                                            Layout.fillWidth: true
                                            elide: Text.ElideMiddle
                                        }
                                        Label {
                                            text: (modelData.stage ? ("[" + modelData.stage + "] ") : "") + (modelData.error || "")
                                            font.pixelSize: 11
                                            color: "#666666"
                                            Layout.preferredWidth: 260
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                Label {
                                    visible: {
                                        if (!healthTab.loaded) return true
                                        var detailed = healthTab.healthData["detailedFailures"] || []
                                        if (detailed.length > 0) return false
                                        var errors = healthTab.healthData["recentErrors"] || []
                                        return errors.length === 0
                                    }
                                    text: healthTab.loaded ? qsTr("No recent errors.") : qsTr("Click Refresh to load error data.")
                                    font.pixelSize: 11; color: "#999999"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Actions")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                RowLayout {
                                    Layout.fillWidth: true
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

                                Label {
                                    visible: healthTab.actionStatusMessage.length > 0
                                    text: healthTab.actionStatusMessage
                                    font.pixelSize: 11
                                    color: healthTab.actionStatusIsError ? "#C62828" : "#2E7D32"
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
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
        onAccepted: {
            var ok = false
            if (serviceManager && serviceManager.rebuildAll) {
                ok = serviceManager.rebuildAll()
            } else if (settingsController) {
                settingsController.rebuildIndex()
                ok = true
            }

            if (ok) {
                healthTab.setActionStatus(qsTr("Rebuild-all request sent."), false)
                healthTab.refreshHealth()
            } else {
                healthTab.setActionStatus(
                    qsTr("Failed to send rebuild-all request. Check service status."),
                    true
                )
            }
        }
    }

    MessageDialog {
        id: rebuildVectorDialog
        title: qsTr("Rebuild Vector Index")
        text: qsTr("Are you sure you want to rebuild the vector index? All embeddings will be regenerated.")
        buttons: MessageDialog.Ok | MessageDialog.Cancel
        onAccepted: {
            var ok = false
            if (serviceManager && serviceManager.rebuildVectorIndex) {
                ok = serviceManager.rebuildVectorIndex()
            } else if (settingsController) {
                settingsController.rebuildVectorIndex()
                ok = true
            }

            if (ok) {
                healthTab.setActionStatus(qsTr("Vector rebuild request sent."), false)
                healthTab.refreshHealth()
            } else {
                healthTab.setActionStatus(
                    qsTr("Failed to send vector rebuild request. Check service status."),
                    true
                )
            }
        }
    }

    MessageDialog {
        id: clearCacheDialog
        title: qsTr("Clear Cache")
        text: qsTr("Are you sure you want to clear the extraction cache? Cached content will need to be re-extracted on next scan.")
        buttons: MessageDialog.Ok | MessageDialog.Cancel
        onAccepted: {
            var ok = false
            if (serviceManager && serviceManager.clearExtractionCache) {
                ok = serviceManager.clearExtractionCache()
            } else if (settingsController) {
                settingsController.clearExtractionCache()
                ok = true
            }

            if (ok) {
                healthTab.setActionStatus(qsTr("Clear-cache request sent."), false)
            } else {
                healthTab.setActionStatus(
                    qsTr("Failed to clear cache. Check service status."),
                    true
                )
            }
        }
    }

    FolderDialog {
        id: reindexFolderDialog
        title: qsTr("Select Folder to Reindex")
        onAccepted: {
            var path = selectedFolder.toString()
            var ok = false
            if (serviceManager && serviceManager.reindexPath) {
                ok = serviceManager.reindexPath(path)
            } else if (settingsController) {
                settingsController.reindexFolder(path)
                ok = true
            }

            if (ok) {
                healthTab.setActionStatus(qsTr("Reindex request sent."), false)
                healthTab.refreshHealth()
            } else {
                healthTab.setActionStatus(
                    qsTr("Failed to send reindex request. Check service status."),
                    true
                )
            }
        }
    }

    Connections {
        target: hotkeyManagerObj
        enabled: hotkeyManagerObj !== null
        function onHotkeyConflictDetected(attemptedHotkey, error, suggestions) {
            settingsWindow.hotkeyConflictMessage = error || qsTr("Hotkey is unavailable.")
            settingsWindow.hotkeySuggestions = suggestions || []
        }
    }

    Connections {
        target: serviceManager
        enabled: serviceManager !== null
        function onServiceError(serviceName, error) {
            healthTab.setActionStatus(
                qsTr("%1 error: %2").arg(serviceName || qsTr("Service")).arg(error || qsTr("Unknown error")),
                true
            )
        }
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
