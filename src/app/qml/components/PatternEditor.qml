import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property var patterns: []
    property var readOnlyPatterns: []

    signal patternsChanged()

    implicitHeight: mainLayout.implicitHeight

    ColumnLayout {
        id: mainLayout
        anchors.fill: parent
        spacing: 6

        // Section header for built-in patterns
        Label {
            text: qsTr("Built-in patterns (%1)").arg(root.readOnlyPatterns.length)
            font.pixelSize: 11
            font.weight: Font.DemiBold
            color: "#666666"
        }

        // Read-only patterns (scrollable list)
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(root.readOnlyPatterns.length * 22 + 8, 110)
            radius: 4
            color: "#F0F0F0"
            border.width: 1
            border.color: "#C0C0C0"

            ScrollView {
                anchors.fill: parent
                anchors.margins: 4
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 2

                    Repeater {
                        model: root.readOnlyPatterns

                        delegate: Label {
                            text: modelData
                            font.pixelSize: 11
                            font.family: "Menlo"
                            color: "#999999"
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        // Section header for user patterns
        Label {
            text: qsTr("User patterns (%1)").arg(root.patterns.length)
            font.pixelSize: 11
            font.weight: Font.DemiBold
            color: "#1A1A1A"
            Layout.topMargin: 8
        }

        // User patterns list
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(root.patterns.length * 28 + 8, 48)
            Layout.maximumHeight: 140
            radius: 4
            color: "#FFFFFF"
            border.width: 1
            border.color: "#C0C0C0"

            ScrollView {
                anchors.fill: parent
                anchors.margins: 4
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 2

                    Repeater {
                        model: root.patterns

                        delegate: RowLayout {
                            required property int index
                            spacing: 4
                            Layout.fillWidth: true

                            Label {
                                text: modelData
                                font.pixelSize: 12
                                font.family: "Menlo"
                                color: "#1A1A1A"
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            // Match count hint
                            Label {
                                text: patternMatchHint(modelData)
                                font.pixelSize: 10
                                color: "#999999"
                                visible: text.length > 0
                            }

                            Button {
                                text: "\u00D7"
                                font.pixelSize: 14
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                                flat: true
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Remove pattern")
                                onClicked: {
                                    var newPatterns = root.patterns.slice()
                                    newPatterns.splice(index, 1)
                                    root.patterns = newPatterns
                                    root.patternsChanged()
                                }
                            }
                        }
                    }

                    Label {
                        visible: root.patterns.length === 0
                        text: qsTr("No custom patterns. Add one below.")
                        font.pixelSize: 11
                        color: "#999999"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        Layout.topMargin: 4
                    }
                }
            }
        }

        // Add pattern row
        RowLayout {
            spacing: 8
            Layout.fillWidth: true

            TextField {
                id: newPatternField
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. *.log, temp/, .env*")
                font.pixelSize: 12
                font.family: "Menlo"

                // Validation indicator
                background: Rectangle {
                    radius: 4
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: {
                        if (newPatternField.text.length === 0) return "#C0C0C0"
                        if (isValidPattern(newPatternField.text)) return "#2E7D32"
                        return "#C62828"
                    }
                }

                Keys.onReturnPressed: addPatternAction()
                Keys.onEnterPressed: addPatternAction()
            }

            Button {
                text: qsTr("Add")
                enabled: newPatternField.text.trim().length > 0 && isValidPattern(newPatternField.text.trim())
                onClicked: addPatternAction()
            }
        }

        // Validation hint
        Label {
            id: validationHint
            visible: newPatternField.text.length > 0
            text: {
                var pat = newPatternField.text.trim()
                if (pat.length === 0) return ""
                if (!isValidPattern(pat)) return qsTr("Invalid pattern syntax")
                if (isDuplicate(pat)) return qsTr("Pattern already exists")
                return patternMatchHint(pat)
            }
            font.pixelSize: 10
            color: {
                var pat = newPatternField.text.trim()
                if (!isValidPattern(pat) || isDuplicate(pat)) return "#C62828"
                return "#2E7D32"
            }
        }
    }

    function addPatternAction() {
        var pattern = newPatternField.text.trim()
        if (pattern.length === 0) return
        if (!isValidPattern(pattern)) return
        if (isDuplicate(pattern)) return

        var newPatterns = root.patterns.slice()
        newPatterns.push(pattern)
        root.patterns = newPatterns
        root.patternsChanged()
        newPatternField.text = ""
    }

    function isValidPattern(pattern) {
        // Basic validation: not empty, no control chars, no unmatched brackets
        if (pattern.length === 0) return false
        if (pattern.length > 256) return false

        // Check for unmatched brackets
        var bracketDepth = 0
        for (var i = 0; i < pattern.length; i++) {
            var ch = pattern.charAt(i)
            if (ch === '[') bracketDepth++
            else if (ch === ']') bracketDepth--
            if (bracketDepth < 0) return false
        }
        if (bracketDepth !== 0) return false

        return true
    }

    function isDuplicate(pattern) {
        // Check user patterns
        for (var i = 0; i < root.patterns.length; i++) {
            if (root.patterns[i] === pattern) return true
        }
        // Check read-only patterns
        for (var j = 0; j < root.readOnlyPatterns.length; j++) {
            if (root.readOnlyPatterns[j] === pattern) return true
        }
        return false
    }

    function patternMatchHint(pattern) {
        // Provide a human-readable hint about what the pattern matches
        if (pattern.endsWith("/")) return qsTr("matches directories")
        if (pattern.startsWith("*.")) {
            var ext = pattern.substring(2)
            return qsTr("matches .%1 files").arg(ext)
        }
        if (pattern.startsWith("**/")) return qsTr("matches at any depth")
        if (pattern.startsWith("!")) return qsTr("negation pattern")
        if (pattern.indexOf("*") >= 0 || pattern.indexOf("?") >= 0) return qsTr("glob pattern")
        return qsTr("exact match")
    }
}
