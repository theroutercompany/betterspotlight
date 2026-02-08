import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: root

    // model: QVariantList of objects { path: string, mode: string }
    // mode values: "index_embed", "index_only", "skip"
    property var model: []

    implicitHeight: mainLayout.implicitHeight

    ColumnLayout {
        id: mainLayout
        anchors.fill: parent
        spacing: 6

        // Directory list
        Repeater {
            model: root.model

            delegate: Rectangle {
                required property var modelData
                required property int index
                Layout.fillWidth: true
                height: 36
                radius: 4
                color: index % 2 === 0 ? "#FFFFFF" : "#F0F0F0"
                border.width: 1
                border.color: "#C0C0C0"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 4
                    spacing: 8

                    Label {
                        text: modelData.path || ""
                        font.pixelSize: 12
                        font.family: "Menlo"
                        color: "#1A1A1A"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }

                    ComboBox {
                        id: modeCombo
                        Layout.preferredWidth: 140
                        model: [
                            qsTr("Index + Embed"),
                            qsTr("Index Only"),
                            qsTr("Skip")
                        ]
                        property var modeValues: ["index_embed", "index_only", "skip"]
                        currentIndex: {
                            var mode = modelData.mode || "index_embed"
                            var idx = modeValues.indexOf(mode)
                            return idx >= 0 ? idx : 0
                        }
                        onActivated: function(comboIndex) {
                            var newModel = root.model.slice()
                            var entry = Object.assign({}, newModel[index])
                            entry.mode = modeValues[comboIndex]
                            newModel[index] = entry
                            root.model = newModel
                        }
                    }

                    Button {
                        text: "\u2212"
                        font.pixelSize: 16
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Remove this directory")
                        onClicked: {
                            var newModel = root.model.slice()
                            newModel.splice(index, 1)
                            root.model = newModel
                        }
                    }
                }
            }
        }

        // Empty state
        Label {
            visible: root.model.length === 0
            text: qsTr("No directories configured. Click \u201cAdd Directory\u201d to get started.")
            font.pixelSize: 12
            color: "#999999"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            Layout.topMargin: 8
            Layout.bottomMargin: 8
        }

        // Add directory button
        RowLayout {
            spacing: 8
            Layout.fillWidth: true

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Add Directory\u2026")
                icon.name: "folder-open"
                onClicked: addFolderDialog.open()
            }
        }
    }

    FolderDialog {
        id: addFolderDialog
        title: qsTr("Select Directory to Index")
        onAccepted: {
            var folderPath = selectedFolder.toString()
            // Strip file:// prefix for display
            if (folderPath.startsWith("file://")) {
                folderPath = folderPath.substring(7)
            }

            // Check for duplicates
            for (var i = 0; i < root.model.length; i++) {
                if (root.model[i].path === folderPath) {
                    return
                }
            }

            var newModel = root.model.slice()
            newModel.push({ path: folderPath, mode: "index_embed" })
            root.model = newModel
        }
    }
}
