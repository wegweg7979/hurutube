import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// The bar under the list: what is playing, how far in, and the controls that
// reach the engine over IPC. Video itself is in mpv's own window -- this is the
// remote control, which is why nothing here draws frames.
Rectangle {
    id: transport

    property var row: null

    implicitHeight: 78
    color: theme.darkBackground

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: theme.muted
        opacity: 0.4
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

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 12

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Text {
                Layout.fillWidth: true
                text: transport.row ? transport.row.title : qsTr("Nothing playing")
                color: theme.brightForeground
                font.pixelSize: 14
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: {
                    if (!transport.row)
                        return qsTr("Search above, or paste a link")
                    const parts = []
                    if (player.hwdec === "vaapi")
                        parts.push(qsTr("GPU decode (VA-API)"))
                    else if (player.playing)
                        parts.push(qsTr("software decode"))
                    if (player.videoDescription.length > 0 && !player.audioOnly)
                        parts.push(player.videoDescription)
                    if (player.audioOnly)
                        parts.push(qsTr("audio only"))
                    if (player.usedCookies)
                        parts.push(qsTr("needed cookies"))
                    return parts.join(" · ")
                }
                color: theme.muted
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }

        Text {
            text: transport.formatTime(player.position) + " / "
                  + (player.duration > 0 ? transport.formatTime(player.duration)
                                         : (transport.row ? transport.row.duration : "--:--"))
            color: theme.foreground
            font.pixelSize: 13
            font.family: theme.fontFamily
        }

        // Seek is a fixed two-step rather than a slider: a slider in a bar this
        // size is hard to hit, and the arrow keys already do the same thing.
        Button {
            flat: true
            text: qsTr("−10s")
            onClicked: player.seekRelative(-10)
        }

        Button {
            flat: true
            highlighted: player.playing && !player.paused
            text: player.playing && !player.paused ? qsTr("Pause") : qsTr("Play")
            onClicked: player.togglePause()
        }

        Button {
            flat: true
            text: qsTr("+10s")
            onClicked: player.seekRelative(10)
        }

        Button {
            flat: true
            text: player.audioOnly ? qsTr("Video") : qsTr("Audio only")
            onClicked: player.audioOnly = !player.audioOnly
        }

        Button {
            flat: true
            text: qsTr("Fullscreen")
            onClicked: player.toggleFullscreen()
        }

        Slider {
            Layout.preferredWidth: 130
            from: 0
            to: 130
            value: player.volume
            onMoved: player.setVolumeFromQml(Math.round(value))
        }
    }
}
