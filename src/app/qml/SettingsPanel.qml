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

                                    Label {
                                        text: qsTr("Enable query router")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.queryRouterEnabled : true
                                        onToggled: { if (settingsController) settingsController.queryRouterEnabled = checked }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Enable fast embedding expert")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.fastEmbeddingEnabled : true
                                        onToggled: { if (settingsController) settingsController.fastEmbeddingEnabled = checked }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Enable dual-index semantic fusion")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.dualEmbeddingFusionEnabled : true
                                        onToggled: { if (settingsController) settingsController.dualEmbeddingFusionEnabled = checked }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Enable reranker cascade")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.rerankerCascadeEnabled : true
                                        onToggled: { if (settingsController) settingsController.rerankerCascadeEnabled = checked }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Enable personalized LTR")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.personalizedLtrEnabled : true
                                        onToggled: { if (settingsController) settingsController.personalizedLtrEnabled = checked }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Query router min confidence")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController
                                                  ? Number(settingsController.queryRouterMinConfidence).toFixed(2)
                                                  : "0.45"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 0.0
                                        to: 1.0
                                        stepSize: 0.01
                                        value: settingsController ? settingsController.queryRouterMinConfidence : 0.45
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.queryRouterMinConfidence = value
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Strong embedding top-K")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? settingsController.strongEmbeddingTopK.toString() : "40"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 10
                                        to: 200
                                        stepSize: 1
                                        value: settingsController ? settingsController.strongEmbeddingTopK : 40
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.strongEmbeddingTopK = Math.round(value)
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Fast embedding top-K")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? settingsController.fastEmbeddingTopK.toString() : "60"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 10
                                        to: 300
                                        stepSize: 1
                                        value: settingsController ? settingsController.fastEmbeddingTopK : 60
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.fastEmbeddingTopK = Math.round(value)
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Reranker stage-1 max")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? settingsController.rerankerStage1Max.toString() : "40"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 4
                                        to: 200
                                        stepSize: 1
                                        value: settingsController ? settingsController.rerankerStage1Max : 40
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.rerankerStage1Max = Math.round(value)
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Reranker stage-2 max")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? settingsController.rerankerStage2Max.toString() : "12"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 4
                                        to: 100
                                        stepSize: 1
                                        value: settingsController ? settingsController.rerankerStage2Max : 12
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.rerankerStage2Max = Math.round(value)
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Enable answer snippet preview (QA-extractive)")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.qaSnippetEnabled : true
                                        onToggled: { if (settingsController) settingsController.qaSnippetEnabled = checked }
                                    }
                                }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Enable automatic vector generation migration")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Switch {
                                        checked: settingsController ? settingsController.autoVectorMigration : true
                                        onToggled: { if (settingsController) settingsController.autoVectorMigration = checked }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Semantic budget (ms)")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? settingsController.semanticBudgetMs.toString() : "70"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 20
                                        to: 200
                                        stepSize: 5
                                        value: settingsController ? settingsController.semanticBudgetMs : 70
                                        onMoved: { if (settingsController) settingsController.semanticBudgetMs = value }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("Rerank budget (ms)")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? settingsController.rerankBudgetMs.toString() : "120"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 40
                                        to: 300
                                        stepSize: 5
                                        value: settingsController ? settingsController.rerankBudgetMs : 120
                                        onMoved: { if (settingsController) settingsController.rerankBudgetMs = value }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("BM25 weight (name)")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? Number(settingsController.bm25WeightName).toFixed(2) : "10.00"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 30
                                        stepSize: 0.1
                                        value: settingsController ? settingsController.bm25WeightName : 10.0
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.bm25WeightName = value
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("BM25 weight (path)")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? Number(settingsController.bm25WeightPath).toFixed(2) : "5.00"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 30
                                        stepSize: 0.1
                                        value: settingsController ? settingsController.bm25WeightPath : 5.0
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.bm25WeightPath = value
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 4
                                    Layout.fillWidth: true

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: qsTr("BM25 weight (content)")
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: settingsController ? Number(settingsController.bm25WeightContent).toFixed(2) : "1.00"
                                            font.pixelSize: 13
                                            color: "#1A1A1A"
                                        }
                                    }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 10
                                        stepSize: 0.05
                                        value: settingsController ? settingsController.bm25WeightContent : 1.0
                                        onMoved: {
                                            if (settingsController) {
                                                settingsController.bm25WeightContent = value
                                            }
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("All model/runtime behavior controls remain visible even when disabled. Runtime model inventory and effective wiring are shown in Index Health. BM25 weights are persisted immediately and apply to lexical ranking after next service/index reload.")
                                    font.pixelSize: 11
                                    color: "#666666"
                                    wrapMode: Text.WordWrap
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

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                RowLayout {
                                    spacing: 12
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Extraction timeout")
                                        font.pixelSize: 13
                                        color: "#1A1A1A"
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        text: {
                                            var ms = extractionTimeoutSlider.value
                                            if (ms < 1000) return Math.round(ms) + " ms"
                                            return (ms / 1000).toFixed(1) + " s"
                                        }
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        color: "#1A1A1A"
                                    }
                                }

                                Slider {
                                    id: extractionTimeoutSlider
                                    Layout.fillWidth: true
                                    from: 1000
                                    to: 120000
                                    stepSize: 500
                                    value: settingsController ? settingsController.extractionTimeoutMs : 30000
                                    onMoved: {
                                        if (settingsController) {
                                            settingsController.extractionTimeoutMs = Math.round(value)
                                        }
                                    }
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
                    property bool indexWorkRunning: {
                        if (!healthData) return false
                        var pending = Number(healthData["queuePending"] || 0)
                        var inProgress = Number(healthData["queueInProgress"] || 0)
                        var preparing = Number(healthData["queuePreparing"] || 0)
                        var writing = Number(healthData["queueWriting"] || 0)
                        var rebuildAll = healthData["queueRebuildRunning"] === true
                        return pending > 0 || inProgress > 0 || preparing > 0 || writing > 0 || rebuildAll
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

                        if (healthTab.vectorRebuildRunning || healthTab.indexWorkRunning) {
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
                                                if (st === "scanning") return "#F57F17"
                                                if (st === "active") return "#2E7D32"
                                                if (st === "idle") return "#999999"
                                                if (st === "paused") return "#1565C0"
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
                                                if (st === "scanning") return qsTr("Scanning")
                                                if (st === "active") return qsTr("Active")
                                                if (st === "idle") return qsTr("Idle")
                                                if (st === "paused") return qsTr("Paused")
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
                                        { label: qsTr("Preparing"), key: "queuePreparing", format: "int" },
                                        { label: qsTr("Writing"), key: "queueWriting", format: "int" },
                                        { label: qsTr("Failed"), key: "queueFailed", format: "int" },
                                        { label: qsTr("Dropped"), key: "queueDropped", format: "int" },
                                        { label: qsTr("Embedding Queue"), key: "queueEmbedding", format: "int" },
                                        { label: qsTr("Scanned"), key: "queueScanned", format: "int" },
                                        { label: qsTr("Total"), key: "queueTotal", format: "int" },
                                        { label: qsTr("Scan Progress"), key: "queueProgressPct", format: "percent" },
                                        { label: qsTr("Paused"), key: "queuePaused", format: "string_bool" },
                                        { label: qsTr("Rebuild-All Status"), key: "queueRebuildStatus", format: "status" }
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
                                                var rss = Number(serviceStats["rssKb"] || 0) * 1024
                                                var cpu = Number(serviceStats["cpuPct"] || 0)
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
                                        .arg((function() {
                                            var hasDetailExists = (bsignore["fileExists"] === true || bsignore["fileExists"] === false)
                                            var exists = hasDetailExists
                                                ? (bsignore["fileExists"] === true)
                                                : (healthTab.healthData["bsignoreFileExists"] === true)
                                            var loaded = (bsignore["loaded"] === true || healthTab.healthData["bsignoreLoaded"] === true)
                                            if (!exists) {
                                                return qsTr("No (file missing)")
                                            }
                                            return loaded ? qsTr("Yes") : qsTr("No")
                                        })())
                                        .arg(Number(bsignore["patternCount"] || healthTab.healthData["bsignorePatternCount"] || 0).toLocaleString())
                                    font.pixelSize: 12
                                    color: "#1A1A1A"
                                }

                                Label {
                                    property var bsignore: (healthTab.healthData["bsignoreDetails"] || {})
                                    text: qsTr("File Exists: %1")
                                        .arg((function() {
                                            if (bsignore["fileExists"] === true || bsignore["fileExists"] === false) {
                                                return bsignore["fileExists"] === true ? qsTr("Yes") : qsTr("No")
                                            }
                                            return healthTab.healthData["bsignoreFileExists"] === true
                                                ? qsTr("Yes")
                                                : qsTr("No")
                                        })())
                                    font.pixelSize: 12
                                    color: "#666666"
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
                            title: qsTr("Runtime Controls (Effective)")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Semantic Search Enabled"), key: "embeddingEnabled" },
                                        { label: qsTr("Query Router Enabled"), key: "queryRouterEnabled" },
                                        { label: qsTr("Query Router Min Confidence"), key: "queryRouterMinConfidence" },
                                        { label: qsTr("Fast Embedding Enabled"), key: "fastEmbeddingEnabled" },
                                        { label: qsTr("Dual-Index Fusion Enabled"), key: "dualEmbeddingFusionEnabled" },
                                        { label: qsTr("Strong Embedding Top-K"), key: "strongEmbeddingTopK" },
                                        { label: qsTr("Fast Embedding Top-K"), key: "fastEmbeddingTopK" },
                                        { label: qsTr("Reranker Cascade Enabled"), key: "rerankerCascadeEnabled" },
                                        { label: qsTr("Reranker Stage-1 Max"), key: "rerankerStage1Max" },
                                        { label: qsTr("Reranker Stage-2 Max"), key: "rerankerStage2Max" },
                                        { label: qsTr("Semantic Budget (ms)"), key: "semanticBudgetMs" },
                                        { label: qsTr("Rerank Budget (ms)"), key: "rerankBudgetMs" },
                                        { label: qsTr("Max File Size (MB)"), key: "maxFileSizeMB" },
                                        { label: qsTr("Extraction Timeout (ms)"), key: "extractionTimeoutMs" },
                                        { label: qsTr("BM25 Name Weight"), key: "bm25WeightName" },
                                        { label: qsTr("BM25 Path Weight"), key: "bm25WeightPath" },
                                        { label: qsTr("BM25 Content Weight"), key: "bm25WeightContent" },
                                        { label: qsTr("Personalized LTR Enabled"), key: "personalizedLtrEnabled" },
                                        { label: qsTr("QA Snippet Enabled"), key: "qaSnippetEnabled" },
                                        { label: qsTr("Auto Vector Migration"), key: "autoVectorMigration" },
                                        { label: qsTr("Semantic Threshold NL Base"), key: "semanticThresholdNaturalLanguageBase" },
                                        { label: qsTr("Semantic Threshold Short Base"), key: "semanticThresholdShortAmbiguousBase" },
                                        { label: qsTr("Semantic Threshold Path/Code Base"), key: "semanticThresholdPathOrCodeBase" },
                                        { label: qsTr("Semantic Threshold Need Scale"), key: "semanticThresholdNeedScale" },
                                        { label: qsTr("Semantic Threshold Min"), key: "semanticThresholdMin" },
                                        { label: qsTr("Semantic Threshold Max"), key: "semanticThresholdMax" },
                                        { label: qsTr("Semantic-Only Floor NL"), key: "semanticOnlyFloorNaturalLanguage" },
                                        { label: qsTr("Semantic-Only Floor Short"), key: "semanticOnlyFloorShortAmbiguous" },
                                        { label: qsTr("Semantic-Only Floor Path/Code"), key: "semanticOnlyFloorPathOrCode" },
                                        { label: qsTr("Strict Lexical Weak Cutoff"), key: "strictLexicalWeakCutoff" },
                                        { label: qsTr("Semantic-Only Cap NL Weak"), key: "semanticOnlyCapNaturalLanguageWeak" },
                                        { label: qsTr("Semantic-Only Cap NL Strong"), key: "semanticOnlyCapNaturalLanguageStrong" },
                                        { label: qsTr("Semantic-Only Cap Short"), key: "semanticOnlyCapShortAmbiguous" },
                                        { label: qsTr("Semantic-Only Cap Path/Code"), key: "semanticOnlyCapPathOrCode" },
                                        { label: qsTr("Semantic-Only Cap Path/Code Divisor"), key: "semanticOnlyCapPathOrCodeDivisor" },
                                        { label: qsTr("Merge Lexical NL Weak"), key: "mergeLexicalWeightNaturalLanguageWeak" },
                                        { label: qsTr("Merge Semantic NL Weak"), key: "mergeSemanticWeightNaturalLanguageWeak" },
                                        { label: qsTr("Merge Lexical NL Strong"), key: "mergeLexicalWeightNaturalLanguageStrong" },
                                        { label: qsTr("Merge Semantic NL Strong"), key: "mergeSemanticWeightNaturalLanguageStrong" },
                                        { label: qsTr("Merge Lexical Path/Code"), key: "mergeLexicalWeightPathOrCode" },
                                        { label: qsTr("Merge Semantic Path/Code"), key: "mergeSemanticWeightPathOrCode" },
                                        { label: qsTr("Merge Lexical Short"), key: "mergeLexicalWeightShortAmbiguous" },
                                        { label: qsTr("Merge Semantic Short"), key: "mergeSemanticWeightShortAmbiguous" },
                                        { label: qsTr("Semantic Safety Weak NL"), key: "semanticOnlySafetySimilarityWeakNatural" },
                                        { label: qsTr("Semantic Safety Default"), key: "semanticOnlySafetySimilarityDefault" },
                                        { label: qsTr("Relaxed Semantic Delta Weak NL"), key: "relaxedSemanticOnlyDeltaWeakNatural" },
                                        { label: qsTr("Relaxed Semantic Delta Default"), key: "relaxedSemanticOnlyDeltaDefault" },
                                        { label: qsTr("Relaxed Semantic Min Weak NL"), key: "relaxedSemanticOnlyMinWeakNatural" },
                                        { label: qsTr("Relaxed Semantic Min Default"), key: "relaxedSemanticOnlyMinDefault" },
                                        { label: qsTr("Semantic Passage Cap NL"), key: "semanticPassageCapNaturalLanguage" },
                                        { label: qsTr("Semantic Passage Cap Other"), key: "semanticPassageCapOther" },
                                        { label: qsTr("Semantic Softmax Temp NL"), key: "semanticSoftmaxTemperatureNaturalLanguage" },
                                        { label: qsTr("Semantic Softmax Temp Other"), key: "semanticSoftmaxTemperatureOther" },
                                        { label: qsTr("Reranker Stage1 Weight Scale"), key: "rerankerStage1WeightScale" },
                                        { label: qsTr("Reranker Stage1 Min Weight"), key: "rerankerStage1MinWeight" },
                                        { label: qsTr("Reranker Stage2 Weight Scale"), key: "rerankerStage2WeightScale" },
                                        { label: qsTr("Reranker Ambiguity Margin"), key: "rerankerAmbiguityMarginThreshold" },
                                        { label: qsTr("Reranker Fallback Elapsed @80"), key: "rerankerFallbackElapsed80Ms" },
                                        { label: qsTr("Reranker Fallback Elapsed @130"), key: "rerankerFallbackElapsed130Ms" },
                                        { label: qsTr("Reranker Fallback Elapsed @180"), key: "rerankerFallbackElapsed180Ms" },
                                        { label: qsTr("Reranker Fallback Cap Default"), key: "rerankerFallbackCapDefault" },
                                        { label: qsTr("Reranker Fallback Cap @80"), key: "rerankerFallbackCapElapsed80" },
                                        { label: qsTr("Reranker Fallback Cap @130"), key: "rerankerFallbackCapElapsed130" },
                                        { label: qsTr("Reranker Fallback Cap @180"), key: "rerankerFallbackCapElapsed180" },
                                        { label: qsTr("Reranker Fallback Budget Cap"), key: "rerankerFallbackBudgetCap" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12
                                        Layout.fillWidth: true

                                        Label {
                                            text: modelData.label + ":"
                                            font.weight: Font.DemiBold
                                            font.pixelSize: 12
                                            color: "#1A1A1A"
                                            Layout.preferredWidth: 220
                                        }
                                        Label {
                                            property var runtimeSettings: (healthTab.healthData["runtimeSettings"] || {})
                                            text: {
                                                var v = runtimeSettings[modelData.key]
                                                if (v === undefined || v === null) return "--"
                                                if (typeof v === "boolean") return v ? "true" : "false"
                                                if (typeof v === "number") {
                                                    if (Math.abs(v - Math.round(v)) < 0.0001) {
                                                        return Math.round(v).toString()
                                                    }
                                                    return Number(v).toFixed(2)
                                                }
                                                return String(v)
                                            }
                                            font.pixelSize: 12
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WrapAnywhere
                                        }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Runtime Components")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: [
                                        { label: qsTr("Query Router Runtime Mode"), key: "queryRouterRuntimeMode" },
                                        { label: qsTr("Query Router Model Declared"), key: "queryRouterModelDeclared" },
                                        { label: qsTr("Query Router Model Active"), key: "queryRouterModelActive" },
                                        { label: qsTr("Strong Embedding Available"), key: "embeddingStrongAvailable" },
                                        { label: qsTr("Strong Embedding Model ID"), key: "embeddingStrongModelId" },
                                        { label: qsTr("Strong Embedding Provider"), key: "embeddingStrongProvider" },
                                        { label: qsTr("Strong Embedding Generation"), key: "embeddingStrongGeneration" },
                                        { label: qsTr("Fast Embedding Available"), key: "embeddingFastAvailable" },
                                        { label: qsTr("Fast Embedding Model ID"), key: "embeddingFastModelId" },
                                        { label: qsTr("Fast Embedding Provider"), key: "embeddingFastProvider" },
                                        { label: qsTr("Fast Embedding Generation"), key: "embeddingFastGeneration" },
                                        { label: qsTr("Fast Cross-Encoder Available"), key: "crossEncoderFastAvailable" },
                                        { label: qsTr("Strong Cross-Encoder Available"), key: "crossEncoderStrongAvailable" },
                                        { label: qsTr("Personalized LTR Available"), key: "personalizedLtrAvailable" },
                                        { label: qsTr("Personalized LTR Model Version"), key: "personalizedLtrModelVersion" },
                                        { label: qsTr("QA Extractive Available"), key: "qaExtractiveAvailable" },
                                        { label: qsTr("QA Preview Mode"), key: "qaPreviewMode" },
                                        { label: qsTr("Vector Store Available"), key: "vectorStoreAvailable" },
                                        { label: qsTr("Strong Vector Index Available"), key: "vectorIndexStrongAvailable" },
                                        { label: qsTr("Fast Vector Index Available"), key: "vectorIndexFastAvailable" },
                                        { label: qsTr("Model Registry Initialized"), key: "modelRegistryInitialized" }
                                    ]

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 12
                                        Layout.fillWidth: true

                                        Label {
                                            text: modelData.label + ":"
                                            font.weight: Font.DemiBold
                                            font.pixelSize: 12
                                            color: "#1A1A1A"
                                            Layout.preferredWidth: 230
                                        }
                                        Label {
                                            property var runtimeComponents: (healthTab.healthData["runtimeComponents"] || {})
                                            text: {
                                                var v = runtimeComponents[modelData.key]
                                                if (v === undefined || v === null) return "--"
                                                if (typeof v === "boolean") return v ? "true" : "false"
                                                return String(v)
                                            }
                                            font.pixelSize: 12
                                            color: "#1A1A1A"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WrapAnywhere
                                        }
                                    }
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Model Manifest Inventory")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: qsTr("Models Directory: ") + (healthTab.healthData["modelsDirResolved"] || "--")
                                    font.pixelSize: 12
                                    color: "#1A1A1A"
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: qsTr("Manifest Path: ") + (healthTab.healthData["manifestPathResolved"] || "--")
                                    font.pixelSize: 12
                                    color: "#1A1A1A"
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: qsTr("Manifest Present: ")
                                          + ((healthTab.healthData["manifestPresent"] === true) ? qsTr("Yes") : qsTr("No"))
                                    font.pixelSize: 12
                                    color: (healthTab.healthData["manifestPresent"] === true) ? "#2E7D32" : "#C62828"
                                }

                                Repeater {
                                    model: healthTab.healthData["modelManifest"] || []

                                    delegate: Rectangle {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        color: {
                                            var state = modelData.runtimeState || ""
                                            if (state === "active") return "#E8F5E9"
                                            if (state === "available_not_selected" || state === "inactive") return "#FFF8E1"
                                            if (state === "unavailable") return "#FFEBEE"
                                            return "#F4F4F4"
                                        }
                                        border.color: "#D0D0D0"
                                        border.width: 1
                                        radius: 4
                                        implicitHeight: entryLayout.implicitHeight + 12

                                        ColumnLayout {
                                            id: entryLayout
                                            anchors.fill: parent
                                            anchors.margins: 6
                                            spacing: 4

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Label {
                                                    text: (modelData.role || "--")
                                                    font.pixelSize: 12
                                                    font.family: "Menlo"
                                                    font.weight: Font.DemiBold
                                                    color: "#1A1A1A"
                                                    Layout.preferredWidth: 210
                                                }
                                                Label {
                                                    text: qsTr("%1 | %2").arg(modelData.task || "unknown").arg(modelData.latencyTier || "unknown")
                                                    font.pixelSize: 11
                                                    color: "#666666"
                                                    Layout.preferredWidth: 150
                                                }
                                                Label {
                                                    text: modelData.runtimeState || "--"
                                                    font.pixelSize: 11
                                                    color: (modelData.runtimeState === "active") ? "#2E7D32"
                                                         : ((modelData.runtimeState === "unavailable") ? "#C62828" : "#8A4B00")
                                                    Layout.fillWidth: true
                                                    horizontalAlignment: Text.AlignRight
                                                }
                                            }

                                            Label {
                                                text: qsTr("Name: %1    Model ID: %2    Generation: %3")
                                                    .arg(modelData.name || "--")
                                                    .arg(modelData.modelId || "--")
                                                    .arg(modelData.generationId || "--")
                                                font.pixelSize: 11
                                                color: "#1A1A1A"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                            Label {
                                                text: qsTr("File: %1").arg(modelData.modelPath || "--")
                                                font.pixelSize: 11
                                                color: "#1A1A1A"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                            Label {
                                                text: qsTr("File Exists: %1    Readable: %2    Size: %3")
                                                    .arg(modelData.modelExists ? "true" : "false")
                                                    .arg(modelData.modelReadable ? "true" : "false")
                                                    .arg(formatHealthValue({size: modelData.modelSizeBytes || 0}, "size", "bytes"))
                                                font.pixelSize: 11
                                                color: (modelData.modelExists && modelData.modelReadable) ? "#2E7D32" : "#C62828"
                                                Layout.fillWidth: true
                                            }
                                            Label {
                                                text: qsTr("Fallback Role: %1    Tokenizer: %2    Max Seq: %3    Dimensions: %4")
                                                    .arg(modelData.fallbackRole || "--")
                                                    .arg(modelData.tokenizer || "--")
                                                    .arg(Number(modelData.maxSeqLength || 0).toLocaleString())
                                                    .arg(Number(modelData.dimensions || 0).toLocaleString())
                                                font.pixelSize: 11
                                                color: "#666666"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                            Label {
                                                visible: (modelData.runtimeReason || "").length > 0
                                                text: qsTr("Runtime Reason: ") + (modelData.runtimeReason || "")
                                                font.pixelSize: 11
                                                color: "#8A4B00"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                        }
                                    }
                                }

                                Label {
                                    visible: !(healthTab.healthData["modelManifest"] || []).length
                                    text: qsTr("No manifest entries available.")
                                    font.pixelSize: 11
                                    color: "#999999"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Environment Overrides")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8

                                Repeater {
                                    model: healthTab.healthData["environmentKnown"] || []

                                    delegate: Rectangle {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        color: modelData.isSet ? "#E3F2FD" : "#F9F9F9"
                                        border.color: "#D0D0D0"
                                        border.width: 1
                                        radius: 4
                                        implicitHeight: envLayout.implicitHeight + 10

                                        ColumnLayout {
                                            id: envLayout
                                            anchors.fill: parent
                                            anchors.margins: 5
                                            spacing: 3

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    text: modelData.key || "--"
                                                    font.family: "Menlo"
                                                    font.pixelSize: 11
                                                    font.weight: Font.DemiBold
                                                    color: "#1A1A1A"
                                                    Layout.fillWidth: true
                                                }
                                                Label {
                                                    text: modelData.isSet ? qsTr("set") : qsTr("default")
                                                    font.pixelSize: 10
                                                    color: modelData.isSet ? "#1565C0" : "#777777"
                                                }
                                            }
                                            Label {
                                                text: modelData.description || ""
                                                font.pixelSize: 11
                                                color: "#666666"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                            Label {
                                                text: qsTr("Effective: %1").arg(modelData.effectiveValue || "--")
                                                font.pixelSize: 11
                                                color: "#1A1A1A"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                            Label {
                                                visible: modelData.isSet
                                                text: qsTr("Raw: %1").arg(modelData.value || "")
                                                font.pixelSize: 10
                                                color: "#777777"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WrapAnywhere
                                            }
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Detected BETTERSPOTLIGHT_* keys in current process environment:")
                                    font.pixelSize: 11
                                    color: "#666666"
                                }

                                Repeater {
                                    model: healthTab.healthData["environmentAll"] || []
                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 8
                                        Layout.fillWidth: true
                                        Label {
                                            text: modelData.key || "--"
                                            font.family: "Menlo"
                                            font.pixelSize: 10
                                            color: "#1A1A1A"
                                            Layout.preferredWidth: 330
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            text: modelData.value || ""
                                            font.pixelSize: 10
                                            color: "#666666"
                                            Layout.fillWidth: true
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }

                                Label {
                                    visible: !(healthTab.healthData["environmentAll"] || []).length
                                    text: qsTr("No BETTERSPOTLIGHT_* environment variables detected.")
                                    font.pixelSize: 11
                                    color: "#999999"
                                }
                            }
                        }

                        GroupBox {
                            Layout.fillWidth: true
                            title: qsTr("Raw Runtime Settings (DB)")

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 6

                                Repeater {
                                    model: {
                                        var raw = healthTab.healthData["runtimeSettingsRaw"] || {}
                                        return Object.keys(raw).sort()
                                    }

                                    delegate: RowLayout {
                                        required property var modelData
                                        spacing: 8
                                        Layout.fillWidth: true

                                        Label {
                                            text: modelData + ":"
                                            font.pixelSize: 11
                                            font.family: "Menlo"
                                            color: "#1A1A1A"
                                            Layout.preferredWidth: 260
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            property var raw: (healthTab.healthData["runtimeSettingsRaw"] || {})
                                            text: (raw[modelData] === undefined || raw[modelData] === null)
                                                  ? "--"
                                                  : String(raw[modelData])
                                            font.pixelSize: 11
                                            color: "#666666"
                                            Layout.fillWidth: true
                                            wrapMode: Text.WrapAnywhere
                                        }
                                    }
                                }

                                Label {
                                    visible: {
                                        var raw = healthTab.healthData["runtimeSettingsRaw"] || {}
                                        return Object.keys(raw).length === 0
                                    }
                                    text: qsTr("No runtime settings found in DB.")
                                    font.pixelSize: 11
                                    color: "#999999"
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "#C0C0C0" }

                                Label {
                                    text: qsTr("Advanced runtime override (writes directly to settings table)")
                                    font.pixelSize: 11
                                    color: "#666666"
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    TextField {
                                        id: runtimeSettingKeyField
                                        Layout.preferredWidth: 220
                                        placeholderText: qsTr("setting key")
                                        font.pixelSize: 11
                                        font.family: "Menlo"
                                    }

                                    TextField {
                                        id: runtimeSettingValueField
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("setting value")
                                        font.pixelSize: 11
                                        font.family: "Menlo"
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Button {
                                        text: qsTr("Set / Update")
                                        enabled: runtimeSettingKeyField.text.trim().length > 0
                                        onClicked: {
                                            if (!settingsController) return
                                            var key = runtimeSettingKeyField.text.trim()
                                            var value = runtimeSettingValueField.text
                                            var ok = settingsController.setRuntimeSetting(key, value)
                                            if (ok) {
                                                healthTab.setActionStatus(qsTr("Updated runtime setting: %1").arg(key), false)
                                                healthTab.refreshHealth()
                                            } else {
                                                healthTab.setActionStatus(qsTr("Failed to update runtime setting."), true)
                                            }
                                        }
                                    }

                                    Button {
                                        text: qsTr("Remove")
                                        enabled: runtimeSettingKeyField.text.trim().length > 0
                                        onClicked: {
                                            if (!settingsController) return
                                            var key = runtimeSettingKeyField.text.trim()
                                            var ok = settingsController.removeRuntimeSetting(key)
                                            if (ok) {
                                                healthTab.setActionStatus(qsTr("Removed runtime setting: %1").arg(key), false)
                                                healthTab.refreshHealth()
                                            } else {
                                                healthTab.setActionStatus(qsTr("Failed to remove runtime setting."), true)
                                            }
                                        }
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
                                        enabled: !healthTab.vectorRebuildRunning
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
        case "string_bool":
            return val ? qsTr("Yes") : qsTr("No")
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
