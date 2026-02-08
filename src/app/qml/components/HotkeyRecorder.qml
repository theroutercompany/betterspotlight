import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string hotkey: "Cmd+Space"
    property bool recording: false

    implicitHeight: recorderLayout.implicitHeight

    RowLayout {
        id: recorderLayout
        anchors.fill: parent
        spacing: 8

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            radius: 4
            color: root.recording ? "#FFF8E1" : "#FFFFFF"
            border.width: 1
            border.color: root.recording ? "#F57F17" : "#C0C0C0"

            Label {
                anchors.centerIn: parent
                text: root.recording ? qsTr("Press key combination\u2026") : root.hotkey
                font.pixelSize: 13
                font.family: root.recording ? undefined : "Menlo"
                color: root.recording ? "#F57F17" : "#1A1A1A"
            }

            MouseArea {
                anchors.fill: parent
                onClicked: {
                    if (!root.recording) {
                        root.recording = true
                        keyCapture.forceActiveFocus()
                    }
                }
            }

            // Invisible item that captures key events while recording
            Item {
                id: keyCapture
                focus: root.recording
                Keys.onPressed: function(event) {
                    if (!root.recording) return

                    // Ignore bare modifier presses
                    if (event.key === Qt.Key_Shift ||
                        event.key === Qt.Key_Control ||
                        event.key === Qt.Key_Alt ||
                        event.key === Qt.Key_Meta) {
                        return
                    }

                    // Cancel on Escape
                    if (event.key === Qt.Key_Escape) {
                        root.recording = false
                        event.accepted = true
                        return
                    }

                    var parts = []

                    // Build modifier string in standard macOS order
                    if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
                    if (event.modifiers & Qt.AltModifier) parts.push("Alt")
                    if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
                    if (event.modifiers & Qt.MetaModifier) parts.push("Cmd")

                    // Map the key to a readable name
                    var keyName = keyToString(event.key)
                    if (keyName.length === 0) {
                        event.accepted = true
                        return
                    }

                    parts.push(keyName)

                    root.hotkey = parts.join("+")
                    root.recording = false
                    event.accepted = true
                }
            }
        }

        Button {
            text: root.recording ? qsTr("Cancel") : qsTr("Record")
            Layout.preferredWidth: 72
            onClicked: {
                if (root.recording) {
                    root.recording = false
                } else {
                    root.recording = true
                    keyCapture.forceActiveFocus()
                }
            }
        }

        Button {
            text: qsTr("Reset")
            Layout.preferredWidth: 60
            enabled: !root.recording
            onClicked: {
                root.hotkey = "Cmd+Space"
            }
        }
    }

    // Convert a Qt key code to a human-readable string
    function keyToString(key) {
        var map = {}
        map[Qt.Key_Space] = "Space"
        map[Qt.Key_Return] = "Return"
        map[Qt.Key_Enter] = "Enter"
        map[Qt.Key_Tab] = "Tab"
        map[Qt.Key_Backspace] = "Backspace"
        map[Qt.Key_Delete] = "Delete"
        map[Qt.Key_Home] = "Home"
        map[Qt.Key_End] = "End"
        map[Qt.Key_PageUp] = "PageUp"
        map[Qt.Key_PageDown] = "PageDown"
        map[Qt.Key_Up] = "Up"
        map[Qt.Key_Down] = "Down"
        map[Qt.Key_Left] = "Left"
        map[Qt.Key_Right] = "Right"
        map[Qt.Key_F1] = "F1"
        map[Qt.Key_F2] = "F2"
        map[Qt.Key_F3] = "F3"
        map[Qt.Key_F4] = "F4"
        map[Qt.Key_F5] = "F5"
        map[Qt.Key_F6] = "F6"
        map[Qt.Key_F7] = "F7"
        map[Qt.Key_F8] = "F8"
        map[Qt.Key_F9] = "F9"
        map[Qt.Key_F10] = "F10"
        map[Qt.Key_F11] = "F11"
        map[Qt.Key_F12] = "F12"
        map[Qt.Key_Minus] = "-"
        map[Qt.Key_Equal] = "="
        map[Qt.Key_BracketLeft] = "["
        map[Qt.Key_BracketRight] = "]"
        map[Qt.Key_Backslash] = "\\"
        map[Qt.Key_Semicolon] = ";"
        map[Qt.Key_Apostrophe] = "'"
        map[Qt.Key_Comma] = ","
        map[Qt.Key_Period] = "."
        map[Qt.Key_Slash] = "/"
        map[Qt.Key_QuoteLeft] = "`"

        if (map[key] !== undefined) {
            return map[key]
        }

        // Letters A-Z
        if (key >= Qt.Key_A && key <= Qt.Key_Z) {
            return String.fromCharCode(key)
        }

        // Digits 0-9
        if (key >= Qt.Key_0 && key <= Qt.Key_9) {
            return String.fromCharCode(key)
        }

        return ""
    }
}
