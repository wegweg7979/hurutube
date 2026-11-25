import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// The app's single window: a search box, the list of results, and the transport
// for whatever is playing. Layout and keys live here; state lives in C++.
//
// Video plays in mpv's own window, not inside this one -- see src/player.h for
// why. So this window is a browser and a remote control, and it stays exactly
// this size while the video is full screen.
ApplicationWindow {
    id: window

    width: 1120
    height: 760
    // Low on purpose: this app is tiled on a tiling compositor, and a window
    // whose minimum fights the layout gets clipped instead of reported.
    minimumWidth: 620
    minimumHeight: 380
    visible: true
    title: qsTr("hurutube")
    color: theme.darkerBackground

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius > 0 ? Material.Medium : Material.NotRounded

    // The row currently loaded in the engine.
    property var current: null
    property string statusLine: ""
    property bool statusIsError: false
    property bool helpOpen: false
    property bool historyOpen: false

    ListModel { id: results }

    function formatTime(seconds) {
        const total = Math.max(0, Math.floor(seconds))
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const rest = total % 60
        const pad = function (value) { return value < 10 ? "0" + value : "" + value }
        return hours > 0 ? hours + ":" + pad(minutes) + ":" + pad(rest)
                         : minutes + ":" + pad(rest)
    }

    function setStatus(message, isError) {
        window.statusLine = message
        window.statusIsError = isError === true
    }

    function runSearch(text) {
        const query = text.trim()
        if (query.length === 0)
            return
        window.setStatus(qsTr("searching…"), false)
        // One entry point for everything the user can type: the backend decides
        // whether it is a phrase, a video, a playlist, a channel -- or a Shorts
        // link, which it refuses.
        youtube.open(query)
    }

    function showResults(entries, heading) {
        results.clear()
        for (let i = 0; i < entries.length; ++i)
            results.append(entries[i])
        list.currentIndex = results.count > 0 ? 0 : -1
        window.setStatus(entries.length === 1 ? qsTr("%1 result").arg(entries.length)
                                              : qsTr("%1 results — Shorts are not listed").arg(entries.length),
                         false)
        window.presolveCurrent()
        // Hand the keyboard to the list: otherwise the next Enter goes back into
        // the search field and searches again -- which is exactly what the user
        // does NOT mean by pressing Enter right after a search. `/` or Ctrl+L
        // returns to the field.
        if (results.count > 0)
            list.forceActiveFocus()
    }

    function playRow(row, restart) {
        if (!row || !row.url)
            return
        window.current = row
        history.remember(row)
        const start = restart === true ? 0 : history.positionFor(row.id)
        player.play(row.url, start, row.title)
        if (start > 0)
            window.setStatus(qsTr("resuming “%1” at %2").arg(row.title).arg(window.formatTime(start)), false)
        else
            window.setStatus(qsTr("playing “%1”").arg(row.title), false)
    }

    // The row's own URL, so the next one can be resolved while this one plays.
    function presolveCurrent() {
        if (list.currentIndex < 0 || list.currentIndex >= results.count)
            return
        player.presolve(results.get(list.currentIndex).url)
    }

    function playNext(step) {
        if (results.count === 0)
            return
        const next = list.currentIndex + step
        if (next < 0 || next >= results.count)
            return
        list.currentIndex = next
        window.playRow(results.get(next), true)
    }

    Component.onCompleted: {
        player.audioOnly = startAudioOnly
        if (initialQuery.length > 0) {
            searchField.text = initialQuery
            window.runSearch(initialQuery)
        } else if (initialUrl.length > 0) {
            searchField.text = initialUrl
            youtube.open(initialUrl)
        }
        searchField.forceActiveFocus()
    }

    // The position is written every few seconds rather than on every change: a
    // disk write per frame would be absurd, and losing a few seconds of position
    // to a crash is not worth a journal.
    Timer {
        interval: 5000
        repeat: true
        running: player.playing && window.current !== null && player.duration > 0
        onTriggered: {
            if (window.current)
                history.setPosition(window.current.id, player.position, player.duration)
        }
    }

    onClosing: {
        if (window.current && player.duration > 0)
            history.setPosition(window.current.id, player.position, player.duration)
    }

    Connections {
        target: youtube

        function onResultsReady(entries, heading) {
            window.showResults(entries, heading)
        }

        function onVideoReady(entry) {
            results.clear()
            results.append(entry)
            list.currentIndex = 0
            window.playRow(entry, true)
        }

        function onFailed(message) {
            window.setStatus(message, true)
        }

        function onShortsRefused(id) {
            window.setStatus(qsTr("Shorts are blocked by design — /shorts/%1 was not opened.").arg(id), true)
        }
    }

    Connections {
        target: player

        function onFinished() {
            window.playNext(1)
        }

        // The video window was closed. That is the normal way to stop watching,
        // so the now-playing bar empties and the position the engine last
        // reported stays in the history -- no error, no complaint.
        function onEngineClosed(wasPlaying, position, duration) {
            if (window.current !== null && duration > 0)
                history.setPosition(window.current.id, position, duration)
            window.setStatus(wasPlaying ? qsTr("video closed") : qsTr("playback finished"), false)
        }

        function onLastErrorChanged() {
            if (player.lastError.length > 0)
                window.setStatus(player.lastError, true)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 62
            color: theme.darkBackground

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    // Stated rather than left to the row: a Material TextField
                    // floats its placeholder up as a label, and a field stretched
                    // to the header's full height clips that label off the top.
                    Layout.preferredHeight: 46
                    Layout.alignment: Qt.AlignVCenter
                    placeholderText: qsTr("Search YouTube, or paste a link")
                    selectByMouse: true
                    font.pixelSize: 14
                    onAccepted: window.runSearch(text)
                }

                Text {
                    visible: youtube.busy
                    text: qsTr("working…")
                    color: theme.muted
                    font.pixelSize: 12
                }

                Button {
                    flat: true
                    text: "?"
                    onClicked: window.helpOpen = !window.helpOpen
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.topMargin: 8
            Layout.bottomMargin: 6
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: youtube.heading.length > 0 ? youtube.heading
                                                 : qsTr("Search, or paste a link — including a channel or playlist")
                color: theme.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // Which YouTube session this is, said plainly. Muted only when it is a
            // real session, yellow otherwise: an app that cannot tell "signed in"
            // from "found a cookies file" is how you end up debugging the wrong
            // thing, so the words come from what is inside the source.
            Text {
                visible: cookieStatus.length > 0
                text: cookieStatus
                color: cookieStatus.indexOf(qsTr("signed in")) === 0 ? theme.muted : theme.yellow
                font.pixelSize: 12
                elide: Text.ElideMiddle
                Layout.maximumWidth: Math.round(window.width * 0.42)
                Layout.alignment: Qt.AlignVCenter
            }
        }

        // The list and the message that replaces it share one slot: a hidden
        // Text still occupies layout height in QML, so leaving the empty-state
        // message as a sibling row squeezed the status line at small sizes.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: results
                spacing: 2
                delegate: VideoDelegate {}

                onCurrentIndexChanged: window.presolveCurrent()
            }

            // Enter plays the highlighted row; a double click does the same,
            // because a list that only answers the keyboard is a guessing game
            // for anyone with a mouse.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onDoubleClicked: {
                    if (list.currentIndex >= 0 && list.currentIndex < results.count)
                        window.playRow(results.get(list.currentIndex), false)
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 60
                visible: results.count === 0 && !youtube.busy
                text: qsTr("Nothing here yet. Type to search, paste a youtube.com link, "
                           + "or name a channel (@handle) to list its videos.")
                color: theme.muted
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: window.statusLine.length > 0 ? statusLine.implicitHeight + 12 : 0

            Text {
                id: statusLine
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                text: window.statusLine
                color: window.statusIsError ? theme.red : theme.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        Transport {
            Layout.fillWidth: true
            row: window.current
        }
    }

    HelpOverlay {
        id: helpOverlay
        anchors.fill: parent
        visible: window.helpOpen
        onDismissed: window.helpOpen = false
    }

    HistoryPane {
        id: historyPane
        anchors.fill: parent
        visible: window.historyOpen
        onDismissed: window.historyOpen = false
        onPlayRequested: function (entry) {
            window.historyOpen = false
            window.playRow(entry, false)
        }
    }

    // Toggles and actions all set autoRepeat: false -- a held key must not flap
    // a mode (measured: one long Enter counted as "watched 2 times", and a held
    // Space toggles pause over and over). Seeking is the one exception: repeats
    // there are what scrubbing means.
    Shortcut {
        sequence: "Esc"
        autoRepeat: false
        onActivated: {
            if (window.helpOpen)
                window.helpOpen = false
            else if (window.historyOpen)
                window.historyOpen = false
            else
                searchField.text = ""
        }
    }

    Shortcut {
        sequences: [StandardKey.Find, "Ctrl+L", "/"]
        autoRepeat: false
        onActivated: searchField.forceActiveFocus()
    }

    Shortcut {
        sequence: "Space"
        autoRepeat: false
        enabled: !searchField.activeFocus && !window.helpOpen && !window.historyOpen
        onActivated: {
            // Space is the play/pause key in both senses: with nothing loaded it
            // starts the highlighted row -- the thing Return used to be needed
            // for -- and with something loaded it pauses or resumes. `playing`
            // means "the engine has a file" (it tracks idle-active), so it stays
            // true while paused, which is what makes the toggle branch right; the
            // engine going idle at the end of a video makes Space start the next
            // highlighted row instead.
            if (player.playing)
                player.togglePause()
            else if (list.currentIndex >= 0 && list.currentIndex < results.count)
                window.playRow(results.get(list.currentIndex), false)
        }
    }

    Shortcut {
        sequence: "Left"
        enabled: !searchField.activeFocus && window.current !== null
        onActivated: player.seekRelative(-10)
    }

    Shortcut {
        sequence: "Right"
        enabled: !searchField.activeFocus && window.current !== null
        onActivated: player.seekRelative(10)
    }

    Shortcut {
        sequence: "Down"
        autoRepeat: false
        enabled: !searchField.activeFocus && !window.historyOpen && !window.helpOpen
        onActivated: list.incrementCurrentIndex()
    }

    Shortcut {
        sequence: "Up"
        autoRepeat: false
        enabled: !searchField.activeFocus && !window.historyOpen && !window.helpOpen
        onActivated: list.decrementCurrentIndex()
    }

    Shortcut {
        sequence: "Return"
        autoRepeat: false
        enabled: !searchField.activeFocus && !window.historyOpen && !window.helpOpen
        onActivated: {
            if (list.currentIndex >= 0 && list.currentIndex < results.count)
                window.playRow(results.get(list.currentIndex), false)
        }
    }

    Shortcut {
        sequence: "R"
        autoRepeat: false
        enabled: window.current !== null
        onActivated: window.playRow(window.current, true)
    }

    Shortcut {
        sequence: "A"
        autoRepeat: false
        enabled: !searchField.activeFocus
        onActivated: player.audioOnly = !player.audioOnly
    }

    Shortcut {
        sequence: "F"
        autoRepeat: false
        enabled: !searchField.activeFocus
        onActivated: player.toggleFullscreen()
    }

    Shortcut {
        sequence: "Ctrl+H"
        autoRepeat: false
        onActivated: window.historyOpen = !window.historyOpen
    }

    Shortcut {
        sequence: "?"
        autoRepeat: false
        onActivated: window.helpOpen = !window.helpOpen
    }

    Shortcut {
        sequence: "Q"
        autoRepeat: false
        enabled: !searchField.activeFocus
        onActivated: Qt.quit()
    }

    // The history pane's own keys, live only while it is open, so they do not
    // shadow the list's.
    Shortcut {
        sequence: "Up"
        autoRepeat: false
        enabled: window.historyOpen
        onActivated: historyPane.move(-1)
    }

    Shortcut {
        sequence: "Down"
        autoRepeat: false
        enabled: window.historyOpen
        onActivated: historyPane.move(1)
    }

    Shortcut {
        sequence: "Return"
        autoRepeat: false
        enabled: window.historyOpen
        onActivated: historyPane.activate()
    }

    Shortcut {
        sequence: "Delete"
        autoRepeat: false
        enabled: window.historyOpen
        onActivated: historyPane.forget()
    }

    Shortcut {
        sequence: "C"
        autoRepeat: false
        enabled: window.historyOpen
        onActivated: historyPane.clearAll()
    }
}
