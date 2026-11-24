import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// What has been watched, newest first, with how far in the user got. Enter
// resumes; Delete forgets one entry; C twice erases the lot.
Rectangle {
    id: pane

    property bool confirmingClear: false

    signal dismissed()
    signal playRequested(var entry)

    color: Qt.rgba(0, 0, 0, 0.65)

    MouseArea {
        anchors.fill: parent
        onClicked: pane.dismissed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 760)
        height: Math.min(parent.height - 80, 560)
        color: theme.darkBackground
        radius: Math.min(6, theme.radius)
        border.color: theme.muted
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 10

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: qsTr("Watched")
                    color: theme.accent
                    font.pixelSize: 17
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: history.count === 1 ? qsTr("%1 video").arg(history.count)
                                              : qsTr("%1 videos").arg(history.count)
                    color: theme.muted
                    font.pixelSize: 12
                }
            }

            Text {
                Layout.fillWidth: true
                visible: history.count === 0
                text: qsTr("Nothing yet. Anything you play shows up here, with its position.")
                color: theme.muted
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }

            ListView {
                id: historyList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: history.entries
                currentIndex: count > 0 ? 0 : -1
                spacing: 2

                delegate: Item {
                    id: historyRow
                    required property int index
                    required property var modelData

                    width: historyList.width
                    height: 52

                    Rectangle {
                        anchors.fill: parent
                        color: ListView.isCurrentItem ? theme.selection : "transparent"

                        Rectangle {
                            width: 2
                            height: parent.height - 12
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            color: theme.accent
                            visible: ListView.isCurrentItem
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            historyList.currentIndex = historyRow.index
                            pane.playRequested(historyRow.modelData)
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                text: historyRow.modelData.title
                                color: ListView.isCurrentItem ? theme.brightForeground : theme.foreground
                                font.pixelSize: 14
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                text: {
                                    const parts = []
                                    if (historyRow.modelData.channel)
                                        parts.push(historyRow.modelData.channel)
                                    if (historyRow.modelData.plays > 1)
                                        parts.push(qsTr("watched %1 times").arg(historyRow.modelData.plays))
                                    return parts.join(" · ")
                                }
                                color: theme.muted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            text: historyRow.modelData.position > 0
                                  ? qsTr("resume %1").arg(pane.formatTime(historyRow.modelData.position))
                                  : qsTr("from the start")
                            color: theme.muted
                            font.pixelSize: 12
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: pane.confirmingClear
                      ? qsTr("Press C again to erase all %1 entries.").arg(history.count)
                      : qsTr("Enter resume · Delete forget · C clear all · Esc close")
                color: pane.confirmingClear ? theme.yellow : theme.muted
                font.pixelSize: 11
            }
        }
    }

    function formatTime(seconds) {
        const total = Math.max(0, Math.floor(seconds))
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const rest = total % 60
        const pad = function (value) { return value < 10 ? "0" + value : "" + value }
        return hours > 0 ? hours + ":" + pad(minutes) + ":" + pad(rest)
                         : minutes + ":" + pad(rest)
    }

    // How the history pane drives the list it owns.
    function move(step) {
        const next = historyList.currentIndex + step
        if (next < 0 || next >= history.count)
            return
        historyList.currentIndex = next
    }

    function activate() {
        const entry = pane.currentEntry()
        if (entry)
            pane.playRequested(entry)
    }

    function forget() {
        const entry = pane.currentEntry()
        if (!entry)
            return
        history.remove(entry.id)
        if (historyList.currentIndex >= history.count)
            historyList.currentIndex = history.count - 1
    }

    function clearAll() {
        if (!pane.confirmingClear) {
            // Erasing the whole history is not undoable, so it takes two
            // keystrokes rather than one.
            pane.confirmingClear = true
            return
        }
        history.clear()
        pane.confirmingClear = false
    }

    function currentEntry() {
        if (historyList.currentIndex < 0 || historyList.currentIndex >= history.count)
            return null
        return history.entries[historyList.currentIndex]
    }
}
