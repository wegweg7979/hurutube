import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// One row of a result list: thumbnail, title, channel, and a hint of how far
// into it the user got last time.
Item {
    id: delegate

    required property int index
    required property string id
    required property string title
    required property string channel
    required property string duration
    required property string views
    required property string published
    required property bool live
    required property string thumbnail
    required property string url
    required property double seconds

    // Read once, when the row is created: a resume marker does not need to be
    // live, and asking the history for every row on every repaint would be.
    readonly property double savedPosition: history.positionFor(delegate.id)
    readonly property bool watched: savedPosition > 0

    implicitHeight: 84
    width: ListView.view ? ListView.view.width : parent.width

    Rectangle {
        anchors.fill: parent
        color: ListView.isCurrentItem ? theme.selection : (hover.hovered ? theme.background : "transparent")

        // The accent strip marks the row the keyboard is on, so a list with a
        // subtle selection colour still shows where Enter will go.
        Rectangle {
            width: 2
            height: parent.height - 16
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 4
            color: theme.accent
            visible: ListView.isCurrentItem
        }
    }

    HoverHandler { id: hover }

    // The mouse, which the list used to ignore entirely. The delegate is its own
    // component, so it cannot reach the window's functions (an id from another
    // file is not in scope) -- it reports what happened, and Main.qml decides.
    // TapHandler rather than MouseArea so a drag that scrolls cannot be mistaken
    // for a click.
    signal rowClicked(int index)
    signal rowDoubleClicked(int index)

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: delegate.rowClicked(delegate.index)
        onDoubleTapped: delegate.rowDoubleClicked(delegate.index)
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 10
        anchors.bottomMargin: 10
        spacing: 14

        Item {
            Layout.preferredWidth: 118
            Layout.preferredHeight: 66

            Image {
                anchors.fill: parent
                source: delegate.thumbnail
                sourceSize.width: 236
                sourceSize.height: 132
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                clip: true
                // A thumbnail that never arrives must not leave a hole: the
                // placeholder is the row's own background.
                Rectangle {
                    anchors.fill: parent
                    color: theme.background
                    visible: parent.status !== Image.Ready
                }
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: theme.muted
                border.width: 1
                radius: Math.min(4, theme.radius)
            }

            // How far in the user got, drawn on the thumbnail because that is
            // where the eye already is.
            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width * (delegate.watched && delegate.seconds > 0
                                       ? Math.min(1, delegate.savedPosition / delegate.seconds) : 0)
                height: 3
                color: theme.accent
                visible: delegate.watched
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Text {
                Layout.fillWidth: true
                text: delegate.title
                color: ListView.isCurrentItem ? theme.brightForeground : theme.foreground
                font.pixelSize: 15
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: delegate.channel
                    color: theme.muted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    Layout.maximumWidth: 260
                }

                Text {
                    visible: delegate.views.length > 0
                    text: "· " + delegate.views
                    color: theme.muted
                    font.pixelSize: 12
                }

                Text {
                    visible: delegate.published.length > 0
                    text: "· " + delegate.published
                    color: theme.muted
                    font.pixelSize: 12
                }

                Text {
                    visible: delegate.watched
                    text: qsTr("· resume at %1").arg(delegate.formatTime(delegate.savedPosition))
                    color: theme.accent
                    font.pixelSize: 12
                }
            }
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            spacing: 4

            Rectangle {
                Layout.alignment: Qt.AlignRight
                visible: delegate.live
                radius: Math.min(4, theme.radius)
                color: theme.red
                implicitWidth: liveLabel.implicitWidth + 12
                implicitHeight: liveLabel.implicitHeight + 4
                Text {
                    id: liveLabel
                    anchors.centerIn: parent
                    text: qsTr("LIVE")
                    color: theme.darkerBackground
                    font.pixelSize: 11
                    font.bold: true
                }
            }

            Text {
                Layout.alignment: Qt.AlignRight
                visible: delegate.duration.length > 0
                text: delegate.duration
                color: theme.muted
                font.pixelSize: 12
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
}
