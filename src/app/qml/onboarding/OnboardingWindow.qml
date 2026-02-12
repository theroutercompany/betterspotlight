import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: onboardingWindow

    property int currentStep: 0

    title: qsTr("BetterSpotlight Setup")
    width: 580
    height: 500
    minimumWidth: 580
    minimumHeight: 500
    visible: true
    flags: Qt.Window | Qt.WindowTitleHint | Qt.WindowCloseButtonHint

    // Center on screen
    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    Rectangle {
        anchors.fill: parent
        color: "#F0F0F0"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Step indicator bar
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                color: "#E8E8E8"

                RowLayout {
                    anchors.centerIn: parent
                    spacing: 24

                    Repeater {
                        model: [
                            qsTr("Welcome"),
                            qsTr("Full Disk Access"),
                            qsTr("Home Folders"),
                            qsTr("Models")
                        ]

                        delegate: RowLayout {
                            required property int index
                            required property string modelData
                            spacing: 8

                            Rectangle {
                                width: 24
                                height: 24
                                radius: 12
                                color: index <= currentStep ? "#1A1A1A" : "#C0C0C0"

                                Label {
                                    anchors.centerIn: parent
                                    text: String(index + 1)
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: "#FFFFFF"
                                }
                            }

                            Label {
                                text: modelData
                                font.pixelSize: 13
                                font.weight: index === currentStep ? Font.DemiBold : Font.Normal
                                color: index <= currentStep ? "#1A1A1A" : "#999999"
                            }
                        }
                    }
                }
            }

            // Separator
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: "#C0C0C0"
            }

            // Step content area
            StackLayout {
                id: stepStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: currentStep

                WelcomeStep {
                    onNext: {
                        currentStep = 1
                    }
                }

                FdaStep {
                    onNext: {
                        currentStep = 2
                    }
                    onBack: {
                        currentStep = 0
                    }
                }

                HomeMapStep {
                    onBack: {
                        currentStep = 1
                    }
                    onFinished: {
                        currentStep = 3
                    }
                }

                ModelSetupStep {
                    onBack: {
                        currentStep = 2
                    }
                    onFinished: {
                        onboardingControllerObj.completeOnboarding()
                        onboardingWindow.close()
                    }
                }
            }
        }
    }
}
