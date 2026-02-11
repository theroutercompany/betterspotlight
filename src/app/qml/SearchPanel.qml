import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: searchWindow

    property var searchController: null

    width: 680
    height: implicitContentHeight
    flags: Qt.Dialog | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: false

    // Position: centered horizontally, ~200px from top
    x: (Screen.width - width) / 2
    y: 200

    // Maximum height for the results area
    readonly property int maxResultsVisible: 10
    readonly property int resultItemHeight: 56
    readonly property int headerItemHeight: 30
    readonly property int maxResultsHeight: maxResultsVisible * resultItemHeight

    function rowHeight(row) {
        if (!row) return resultItemHeight
        if ((row.rowType || "") === "header") return headerItemHeight
        var item = row.itemData || ({})
        var hasAnswer = (item.answerSnippet || "").length > 0
        var loadingAnswer = (item.answerStatus || "") === "loading"
        return (hasAnswer || loadingAnswer) ? 86 : resultItemHeight
    }

    function computedResultsHeight() {
        if (!searchController || !searchController.resultRows) return 0
        var rows = searchController.resultRows
        var total = 0
        for (var i = 0; i < rows.length; ++i) {
            total += rowHeight(rows[i])
        }
        return Math.min(total, maxResultsHeight)
    }

    // Computed total height
    readonly property int containerPadding: 8
    readonly property int implicitContentHeight: {
        var h = containerPadding * 2 + searchField.height
        if (resultsList.count > 0) {
            h += resultsDivider.height
            h += computedResultsHeight()
        }
        return Math.min(h, 800)
    }

    // Guard: don't auto-dismiss during the show transition
    property bool _dismissEnabled: false

    // Auto-close when focus is lost (but not during the initial show)
    onActiveChanged: {
        if (!active && visible && _dismissEnabled) {
            dismiss()
        }
    }

    function showAndActivate() {
        _dismissEnabled = false
        visible = true
        raise()
        requestActivate()
        searchField.forceActiveFocus()
        searchField.selectAll()
        // Enable auto-dismiss after a short delay to avoid the show→deactivate race
        Qt.callLater(function() { _dismissEnabled = true })
    }

    function dismiss() {
        _dismissEnabled = false
        visible = false
        if (searchController) {
            searchController.clearResults()
        }
    }

    Rectangle {
        id: container
        anchors.fill: parent
        radius: 10
        color: "#F0F0F0"
        border.color: "#C0C0C0"
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: searchWindow.containerPadding
            anchors.bottomMargin: searchWindow.containerPadding
            spacing: 0

            // Search input field
            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 4

                placeholderText: qsTr("Search files...")
                font.pixelSize: 22
                background: Item {}  // Transparent background; the container provides it

                color: "#1A1A1A"
                placeholderTextColor: "#999999"
                selectByMouse: true

                onTextChanged: {
                    if (searchController) {
                        searchController.query = text
                    }
                }

                Keys.onEscapePressed: {
                    searchWindow.dismiss()
                }

                Keys.onDownPressed: {
                    if (searchController && searchController.resultRows.length > 0) {
                        searchController.moveSelection(1)
                        if (searchController.selectedIndex >= 0) {
                            resultsList.positionViewAtIndex(searchController.selectedIndex, ListView.Contain)
                        }
                    }
                }

                Keys.onUpPressed: {
                    if (searchController && searchController.resultRows.length > 0) {
                        searchController.moveSelection(-1)
                        if (searchController.selectedIndex >= 0) {
                            resultsList.positionViewAtIndex(searchController.selectedIndex, ListView.Contain)
                        }
                    }
                }

                Keys.onReturnPressed: {
                    if (searchController && searchController.selectedIndex >= 0) {
                        searchController.openResult(searchController.selectedIndex)
                        searchWindow.dismiss()
                    }
                }

                Keys.onPressed: function(event) {
                    // Cmd+R: reveal in Finder
                    if (event.key === Qt.Key_R && (event.modifiers & Qt.ControlModifier)) {
                        if (searchController && searchController.selectedIndex >= 0) {
                            searchController.revealInFinder(searchController.selectedIndex)
                            searchWindow.dismiss()
                        }
                        event.accepted = true
                    }
                    // Cmd+Shift+C: copy path
                    else if (event.key === Qt.Key_C &&
                             (event.modifiers & Qt.ControlModifier) &&
                             (event.modifiers & Qt.ShiftModifier)) {
                        if (searchController && searchController.selectedIndex >= 0) {
                            searchController.copyPath(searchController.selectedIndex)
                        }
                        event.accepted = true
                    }
                    // Cmd+Shift+A: generate answer preview for selected result.
                    else if (event.key === Qt.Key_A &&
                             (event.modifiers & Qt.ControlModifier) &&
                             (event.modifiers & Qt.ShiftModifier)) {
                        if (searchController && searchController.selectedIndex >= 0) {
                            searchController.requestAnswerSnippet(searchController.selectedIndex)
                        }
                        event.accepted = true
                    }
                }
            }

            // Divider between search field and results
            Rectangle {
                id: resultsDivider
                Layout.fillWidth: true
                Layout.topMargin: 4
                height: 1
                color: "#D0D0D0"
                visible: resultsList.count > 0
            }

            // Search results list
            ListView {
                id: resultsList
                Layout.fillWidth: true
                Layout.preferredHeight: searchWindow.computedResultsHeight()
                visible: count > 0
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                model: searchController ? searchController.resultRows : []
                currentIndex: searchController ? searchController.selectedIndex : -1

                delegate: ResultItem {
                    width: resultsList.width
                    height: (modelData.rowType || "") === "header" ? 30 : searchWindow.resultItemHeight
                    itemData: modelData.itemData || ({})
                    isHeader: (modelData.rowType || "") === "header"
                    headerText: modelData.title || ""
                    isSelected: !isHeader && (index === resultsList.currentIndex)

                    onClicked: {
                        if (searchController && !isHeader) {
                            searchController.selectedIndex = index
                            searchController.openResult(index)
                            searchWindow.dismiss()
                        }
                    }
                }

                // Subtle scrollbar
                ScrollBar.vertical: ScrollBar {
                    policy: resultsList.contentHeight > resultsList.height
                            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }
            }

            // Loading indicator
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 8
                Layout.bottomMargin: 8
                running: searchController ? searchController.isSearching : false
                visible: running
                width: 24
                height: 24
            }
        }
    }
}
