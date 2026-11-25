import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every key the window answers to, in one place. ? opens it.
Rectangle {
    id: overlay

    signal dismissed()

    color: Qt.rgba(0, 0, 0, 0.65)

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.dismissed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 620)
        height: Math.min(parent.height - 80, 470)
        color: theme.darkBackground
        radius: Math.min(6, theme.radius)
        border.color: theme.muted
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 10

            Text {
                text: qsTr("Keyboard")
                color: theme.accent
                font.pixelSize: 17
            }

            Repeater {
                model: [
                    { "keys": "/ or Ctrl+L", "what": qsTr("focus the search box") },
                    { "keys": "Enter", "what": qsTr("search, or open a pasted link") },
                    { "keys": "↑ ↓", "what": qsTr("move through the list") },
                    { "keys": "Space", "what": qsTr("play the highlighted row, or pause what is playing") },
                    { "keys": "double click", "what": qsTr("play the row under the cursor") },
                    { "keys": "← →", "what": qsTr("jump 10 seconds") },
                    { "keys": "R", "what": qsTr("restart the video from the beginning") },
                    { "keys": "A", "what": qsTr("audio only") },
                    { "keys": "F", "what": qsTr("fullscreen") },
                    { "keys": "V", "what": qsTr("cycle codec — auto, H.264, VP9, AV1 (this session)") },
                    { "keys": "Ctrl+H", "what": qsTr("what you have watched") },
                    { "keys": "Esc", "what": qsTr("close this, or clear the search box") },
                    { "keys": "Q", "what": qsTr("quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 16

                    Text {
                        Layout.preferredWidth: 120
                        text: modelData.keys
                        color: theme.brightForeground
                        font.pixelSize: 13
                        font.family: theme.fontFamily
                    }

                    Text {
                        Layout.fillWidth: true
                        text: modelData.what
                        color: theme.foreground
                        font.pixelSize: 13
                    }
                }
            }

            Item { Layout.fillHeight: true }

            Text {
                Layout.fillWidth: true
                text: qsTr("Shorts are blocked on purpose: they never appear in results, "
                           + "and a /shorts/ link is refused instead of played.")
                color: theme.muted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }
}
