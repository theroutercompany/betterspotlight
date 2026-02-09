import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: resultItemRoot

    property var itemData: ({})
    property bool isSelected: false

    signal clicked()

    color: isSelected ? "#0078D4" : mouseArea.containsMouse ? "#E8E8E8" : "transparent"
    radius: 4

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        onClicked: resultItemRoot.clicked()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        // File type icon
        Rectangle {
            Layout.preferredWidth: 32
            Layout.preferredHeight: 32
            Layout.alignment: Qt.AlignVCenter
            radius: 6
            color: isSelected ? "#FFFFFF30" : iconColorForKind(itemData.kind || "")

            Text {
                anchors.centerIn: parent
                text: iconForKind(itemData.kind || "")
                font.pixelSize: 16
                color: isSelected ? "#FFFFFF" : "#555555"
            }
        }

        // File info column
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            // File name
            Text {
                Layout.fillWidth: true
                text: itemData.name || ""
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: isSelected ? "#FFFFFF" : "#1A1A1A"
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            // Parent path
            Text {
                Layout.fillWidth: true
                text: itemData.parentPath || ""
                font.pixelSize: 11
                color: isSelected ? "#FFFFFFBB" : "#888888"
                elide: Text.ElideMiddle
                maximumLineCount: 1
            }

            // Snippet (if available)
            Text {
                Layout.fillWidth: true
                text: itemData.snippet || ""
                font.pixelSize: 11
                color: isSelected ? "#FFFFFF99" : "#AAAAAA"
                elide: Text.ElideRight
                maximumLineCount: 1
                visible: text.length > 0
            }
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 4

            // Match type badge
            Rectangle {
                Layout.preferredWidth: matchTypeText.implicitWidth + 12
                Layout.preferredHeight: 18
                Layout.alignment: Qt.AlignRight
                radius: 4
                color: isSelected ? "#FFFFFF30" : "#E8E8E8"
                visible: (itemData.matchType || "").length > 0

                Text {
                    id: matchTypeText
                    anchors.centerIn: parent
                    text: formatMatchType(itemData.matchType || "")
                    font.pixelSize: 9
                    font.weight: Font.Medium
                    color: isSelected ? "#FFFFFFCC" : "#666666"
                }
            }

            Rectangle {
                Layout.preferredWidth: availabilityText.implicitWidth + 12
                Layout.preferredHeight: 18
                Layout.alignment: Qt.AlignRight
                radius: 4
                visible: (itemData.availabilityStatus || "available") !== "available"
                color: isSelected ? "#FFFFFF30" : "#FFE9D6"

                Text {
                    id: availabilityText
                    anchors.centerIn: parent
                    text: formatAvailability(itemData.availabilityStatus || "available")
                    font.pixelSize: 9
                    font.weight: Font.Medium
                    color: isSelected ? "#FFFFFFCC" : "#8A4B00"
                }
            }
        }
    }

    // Helper functions

    function iconForKind(kind: string): string {
        switch (kind) {
        case "directory": return "\uD83D\uDCC1"  // folder
        case "pdf":       return "\uD83D\uDCC4"  // page
        case "image":     return "\uD83D\uDDBC"  // picture
        case "video":     return "\uD83C\uDFAC"  // clapper
        case "audio":     return "\uD83C\uDFB5"  // music
        case "archive":   return "\uD83D\uDCE6"  // package
        case "code":      return "\u2699"         // gear
        default:          return "\uD83D\uDCC4"  // generic document
        }
    }

    function iconColorForKind(kind: string): string {
        switch (kind) {
        case "directory": return "#FFF3E0"
        case "pdf":       return "#FFEBEE"
        case "image":     return "#E8F5E9"
        case "video":     return "#E3F2FD"
        case "audio":     return "#F3E5F5"
        case "archive":   return "#FFF8E1"
        case "code":      return "#E0F2F1"
        default:          return "#F5F5F5"
        }
    }

    function formatMatchType(matchType: string): string {
        switch (matchType) {
        case "ExactName":    return "exact"
        case "PrefixName":   return "prefix"
        case "ContainsName": return "contains"
        case "ExactPath":    return "path"
        case "PrefixPath":   return "path prefix"
        case "Content":      return "content"
        case "Fuzzy":        return "fuzzy"
        default:             return matchType
        }
    }

    function formatAvailability(status: string): string {
        switch (status) {
        case "offline_placeholder": return "offline"
        case "extract_failed":      return "unavailable"
        default:                    return status
        }
    }
}
