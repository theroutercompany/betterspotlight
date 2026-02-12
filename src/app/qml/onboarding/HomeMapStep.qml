import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: homeMapStep

    signal back()
    signal finished()

    // Local model built from controller's homeDirectories
    ListModel {
        id: dirModel
    }

    Component.onCompleted: {
        var dirs = onboardingControllerObj.homeDirectories
        for (var i = 0; i < dirs.length; ++i) {
            dirModel.append({
                "name": dirs[i].name,
                "icon": dirs[i].icon,
                "mode": dirs[i].suggestedMode
            })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 32
        spacing: 16

        // Title
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Home Directory Setup")
            font.pixelSize: 22
            font.weight: Font.Bold
            color: "#1A1A1A"
        }

        // Description
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 460
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Choose how each folder in your home directory should be handled. " +
                        "You can change these settings later.")
            font.pixelSize: 13
            color: "#666666"
            wrapMode: Text.WordWrap
            lineHeight: 1.4
        }

        // Legend row
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 16

            Repeater {
                model: [
                    { label: qsTr("Index + Embed"), desc: qsTr("Full search") },
                    { label: qsTr("Index Only"), desc: qsTr("Name only") },
                    { label: qsTr("Skip"), desc: qsTr("Ignored") }
                ]

                delegate: RowLayout {
                    required property var modelData
                    spacing: 4

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: modelData.label === qsTr("Index + Embed") ? "#2E7D32"
                             : modelData.label === qsTr("Index Only") ? "#F57F17"
                             : "#C0C0C0"
                    }

                    Label {
                        text: modelData.label
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: "#1A1A1A"
                    }

                    Label {
                        text: "(" + modelData.desc + ")"
                        font.pixelSize: 11
                        color: "#999999"
                    }
                }
            }
        }

        // Directory list
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 8
            color: "#F6F6F6"
            border.width: 1
            border.color: "#C0C0C0"

            ListView {
                id: dirListView
                anchors.fill: parent
                anchors.margins: 4
                model: dirModel
                clip: true
                spacing: 1

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                delegate: Rectangle {
                    required property int index
                    required property string name
                    required property string icon
                    required property string mode

                    width: dirListView.width - 8
                    x: 4
                    height: 40
                    radius: 4
                    color: index % 2 === 0 ? "#FFFFFF" : "#FAFAFA"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        // Folder icon
                        Label {
                            text: icon
                            font.pixelSize: 16
                            Layout.preferredWidth: 24
                            horizontalAlignment: Text.AlignHCenter
                        }

                        // Folder name
                        Label {
                            text: name
                            font.pixelSize: 13
                            color: "#1A1A1A"
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        // Mode selector
                        ComboBox {
                            id: modeCombo
                            Layout.preferredWidth: 150
                            model: [
                                qsTr("Index + Embed"),
                                qsTr("Index Only"),
                                qsTr("Skip")
                            ]

                            currentIndex: {
                                switch (mode) {
                                case "index_embed": return 0
                                case "index_only": return 1
                                case "skip": return 2
                                default: return 1
                                }
                            }

                            font.pixelSize: 12

                            onActivated: function(idx) {
                                var modeValues = ["index_embed", "index_only", "skip"]
                                dirModel.setProperty(index, "mode", modeValues[idx])
                            }
                        }
                    }
                }
            }
        }

        // Navigation buttons
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                Layout.preferredWidth: 80
                Layout.preferredHeight: 36
                text: qsTr("Back")
                flat: true

                contentItem: Label {
                    text: parent.text
                    font.pixelSize: 13
                    color: "#666666"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: homeMapStep.back()
            }

            Item { Layout.fillWidth: true }

            Button {
                Layout.preferredWidth: 160
                Layout.preferredHeight: 40
                text: qsTr("Continue")

                contentItem: Label {
                    text: parent.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: "#FFFFFF"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: parent.hovered ? "#3E8E41" : "#2E7D32"

                    Behavior on color { ColorAnimation { duration: 150 } }
                }

                onClicked: {
                    // Build variant list from model
                    var dirs = []
                    for (var i = 0; i < dirModel.count; ++i) {
                        dirs.push({
                            "name": dirModel.get(i).name,
                            "icon": dirModel.get(i).icon,
                            "mode": dirModel.get(i).mode
                        })
                    }
                    onboardingControllerObj.saveHomeMap(dirs)
                    homeMapStep.finished()
                }
            }
        }
    }
}
