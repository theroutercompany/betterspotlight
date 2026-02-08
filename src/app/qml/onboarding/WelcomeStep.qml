import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: welcomeStep

    signal next()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 24

        // Top spacer
        Item { Layout.fillHeight: true; Layout.preferredHeight: 20 }

        // App icon placeholder
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 80
            height: 80
            radius: 18
            color: "#1A1A1A"

            Label {
                anchors.centerIn: parent
                text: "BS"
                font.pixelSize: 28
                font.weight: Font.Bold
                color: "#FFFFFF"
            }
        }

        // Title
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Welcome to BetterSpotlight")
            font.pixelSize: 22
            font.weight: Font.Bold
            color: "#1A1A1A"
        }

        // Description
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 420
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("BetterSpotlight is a fast, private file search tool that works entirely offline. " +
                        "Your files are indexed locally — nothing leaves your Mac.")
            font.pixelSize: 13
            color: "#666666"
            wrapMode: Text.WordWrap
            lineHeight: 1.4
        }

        // FDA explanation
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 420
            Layout.fillWidth: true
            implicitHeight: fdaExplanation.implicitHeight + 24
            radius: 8
            color: "#E8E8E8"

            Label {
                id: fdaExplanation
                anchors.fill: parent
                anchors.margins: 12
                text: qsTr("To search across all your files, BetterSpotlight needs Full Disk Access permission. " +
                            "We'll guide you through granting it in the next step.")
                font.pixelSize: 13
                color: "#666666"
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }
        }

        // Bottom spacer
        Item { Layout.fillHeight: true; Layout.preferredHeight: 20 }

        // Get Started button
        Button {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 200
            Layout.preferredHeight: 40
            text: qsTr("Get Started")

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

            onClicked: welcomeStep.next()
        }

        // Bottom margin
        Item { Layout.preferredHeight: 8 }
    }
}
