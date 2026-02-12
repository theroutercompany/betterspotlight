import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: modelSetupStep

    signal back()
    signal finished()

    property bool downloadStarted: false
    property bool downloadCompleted: false
    property bool downloadSucceeded: false

    function startRecommendedDownload() {
        var roles = [
            "bi-encoder",
            "bi-encoder-fast",
            "cross-encoder-fast",
            "cross-encoder",
            "qa-extractive"
        ]
        var started = serviceManagerObj.downloadModels(roles, false)
        if (started) {
            downloadStarted = true
        }
    }

    Connections {
        target: serviceManagerObj

        function onModelDownloadStateChanged() {
            if (!downloadStarted) {
                return
            }
            if (!serviceManagerObj.modelDownloadRunning) {
                downloadCompleted = true
                downloadSucceeded = !serviceManagerObj.modelDownloadHasError
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 32
        spacing: 18

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Model Setup")
            font.pixelSize: 22
            font.weight: Font.Bold
            color: "#1A1A1A"
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 470
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("BetterSpotlight ships lean and downloads advanced ML models on demand. " +
                       "This keeps install size small while enabling higher relevance.")
            font.pixelSize: 13
            color: "#666666"
            wrapMode: Text.WordWrap
            lineHeight: 1.4
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.maximumWidth: 470
            implicitHeight: statusColumn.implicitHeight + 24
            radius: 8
            color: "#F6F6F6"
            border.width: 1
            border.color: "#C0C0C0"

            ColumnLayout {
                id: statusColumn
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Label {
                    text: qsTr("Recommended:")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: "#1A1A1A"
                }

                Label {
                    text: qsTr("• Strong + fast embeddings")
                    font.pixelSize: 12
                    color: "#666666"
                }

                Label {
                    text: qsTr("• Fast + strong rerankers")
                    font.pixelSize: 12
                    color: "#666666"
                }

                Label {
                    text: qsTr("• Optional extractive QA model")
                    font.pixelSize: 12
                    color: "#666666"
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            Button {
                Layout.preferredWidth: 230
                Layout.preferredHeight: 38
                enabled: !serviceManagerObj.modelDownloadRunning
                text: serviceManagerObj.modelDownloadRunning
                      ? qsTr("Downloading...")
                      : (downloadCompleted && !downloadSucceeded)
                        ? qsTr("Retry Download")
                        : qsTr("Download Recommended Models")

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
                    color: parent.enabled
                           ? (parent.hovered ? "#333333" : "#1A1A1A")
                           : "#B0B0B0"
                }

                onClicked: modelSetupStep.startRecommendedDownload()
            }
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.maximumWidth: 470
            implicitHeight: statusText.implicitHeight + 20
            radius: 6
            color: downloadCompleted
                   ? (downloadSucceeded ? "#E8F5E9" : "#FFEBEE")
                   : (serviceManagerObj.modelDownloadRunning ? "#E3F2FD" : "#F8F8F8")
            border.width: 1
            border.color: downloadCompleted
                          ? (downloadSucceeded ? "#A5D6A7" : "#EF9A9A")
                          : (serviceManagerObj.modelDownloadRunning ? "#90CAF9" : "#D0D0D0")

            Label {
                id: statusText
                anchors.fill: parent
                anchors.margins: 10
                wrapMode: Text.WordWrap
                text: serviceManagerObj.modelDownloadStatus && serviceManagerObj.modelDownloadStatus.length > 0
                      ? serviceManagerObj.modelDownloadStatus
                      : qsTr("No downloads started yet.")
                font.pixelSize: 12
                color: downloadCompleted
                       ? (downloadSucceeded ? "#2E7D32" : "#C62828")
                       : "#555555"
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 470
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("You can continue without downloading. The app will still work with lexical fallback " +
                       "and local bootstrap models.")
            font.pixelSize: 11
            color: "#888888"
            wrapMode: Text.WordWrap
            lineHeight: 1.3
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                Layout.preferredWidth: 80
                Layout.preferredHeight: 36
                text: qsTr("Back")
                flat: true

                onClicked: modelSetupStep.back()
            }

            Item { Layout.fillWidth: true }

            Button {
                Layout.preferredWidth: 170
                Layout.preferredHeight: 40
                text: qsTr("Finish Setup")
                enabled: !serviceManagerObj.modelDownloadRunning

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
                    color: parent.enabled
                           ? (parent.hovered ? "#3E8E41" : "#2E7D32")
                           : "#B0B0B0"
                }

                onClicked: modelSetupStep.finished()
            }
        }
    }
}
