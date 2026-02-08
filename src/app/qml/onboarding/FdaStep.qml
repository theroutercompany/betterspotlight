import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: fdaStep

    signal next()
    signal back()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 32
        spacing: 20

        // Title
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Full Disk Access")
            font.pixelSize: 22
            font.weight: Font.Bold
            color: "#1A1A1A"
        }

        // Instructions
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 460
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("BetterSpotlight needs Full Disk Access to search files across your Mac. " +
                        "This permission stays on your Mac — no data is sent anywhere.")
            font.pixelSize: 13
            color: "#666666"
            wrapMode: Text.WordWrap
            lineHeight: 1.4
        }

        // Step-by-step instructions box
        Rectangle {
            Layout.fillWidth: true
            Layout.maximumWidth: 460
            Layout.alignment: Qt.AlignHCenter
            implicitHeight: instructionsColumn.implicitHeight + 24
            radius: 8
            color: "#F6F6F6"
            border.width: 1
            border.color: "#C0C0C0"

            ColumnLayout {
                id: instructionsColumn
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Label {
                    text: qsTr("How to grant access:")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: "#1A1A1A"
                }

                Label {
                    text: qsTr("1. Click \"Open System Settings\" below")
                    font.pixelSize: 13
                    color: "#666666"
                }

                Label {
                    text: qsTr("2. Find BetterSpotlight in the list")
                    font.pixelSize: 13
                    color: "#666666"
                }

                Label {
                    text: qsTr("3. Toggle the switch to enable access")
                    font.pixelSize: 13
                    color: "#666666"
                }

                Label {
                    text: qsTr("4. Return here and click \"Verify Access\"")
                    font.pixelSize: 13
                    color: "#666666"
                }
            }
        }

        // Action buttons row
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 16

            Button {
                Layout.preferredWidth: 180
                Layout.preferredHeight: 36
                text: qsTr("Open System Settings")

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
                    color: parent.hovered ? "#333333" : "#1A1A1A"

                    Behavior on color { ColorAnimation { duration: 150 } }
                }

                onClicked: {
                    Qt.openUrlExternally("x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")
                }
            }

            Button {
                Layout.preferredWidth: 140
                Layout.preferredHeight: 36
                text: qsTr("Verify Access")

                contentItem: Label {
                    text: parent.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: "#1A1A1A"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: parent.hovered ? "#E0E0E0" : "#F0F0F0"
                    border.width: 1
                    border.color: "#C0C0C0"

                    Behavior on color { ColorAnimation { duration: 150 } }
                }

                onClicked: {
                    onboardingControllerObj.checkFda()
                }
            }
        }

        // Status indicator
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: statusRow.implicitWidth + 24
            Layout.preferredHeight: 32
            radius: 6
            color: onboardingControllerObj.fdaGranted ? "#E8F5E9" : "#FFF8E1"
            border.width: 1
            border.color: onboardingControllerObj.fdaGranted ? "#A5D6A7" : "#FFE082"

            RowLayout {
                id: statusRow
                anchors.centerIn: parent
                spacing: 8

                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    color: onboardingControllerObj.fdaGranted ? "#2E7D32" : "#F57F17"
                }

                Label {
                    text: onboardingControllerObj.fdaGranted
                          ? qsTr("Access granted")
                          : qsTr("Not yet granted")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: onboardingControllerObj.fdaGranted ? "#2E7D32" : "#F57F17"
                }
            }
        }

        // Skip warning
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 400
            horizontalAlignment: Text.AlignHCenter
            visible: !onboardingControllerObj.fdaGranted
            text: qsTr("You can skip this step, but BetterSpotlight won't be able to search " +
                        "all files until Full Disk Access is granted.")
            font.pixelSize: 11
            color: "#999999"
            wrapMode: Text.WordWrap
            lineHeight: 1.3
        }

        // Spacer
        Item { Layout.fillHeight: true }

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

                onClicked: fdaStep.back()
            }

            Item { Layout.fillWidth: true }

            Button {
                visible: !onboardingControllerObj.fdaGranted
                Layout.preferredWidth: 110
                Layout.preferredHeight: 36
                text: qsTr("Skip for now")
                flat: true

                contentItem: Label {
                    text: parent.text
                    font.pixelSize: 13
                    color: "#999999"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: fdaStep.next()
            }

            Button {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 36
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
                    color: parent.hovered ? "#333333" : "#1A1A1A"

                    Behavior on color { ColorAnimation { duration: 150 } }
                }

                onClicked: fdaStep.next()
            }
        }
    }
}
