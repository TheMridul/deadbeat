import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 820
    height: 560
    minimumWidth: 560
    minimumHeight: 380
    visible: true
    title: "Deadbeat"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property string selectedId: ""
    // Relative times re-render from this, not from a timer per row.
    property real now: Date.now() / 1000

    Timer { interval: 15000; running: true; repeat: true; onTriggered: win.now = Date.now() / 1000 }

    function ago(t) {
        // Reads translator.language so the text re-renders on a language switch.
        translator.language
        if (!t)
            return ""
        const s = Math.max(0, Math.round(win.now - t))
        if (s < 60)
            return qsTr("just now")
        if (s < 3600)
            return qsTr("%n min ago", "", Math.floor(s / 60))
        if (s < 86400)
            return qsTr("%n h ago", "", Math.floor(s / 3600))
        return qsTr("%n d ago", "", Math.floor(s / 86400))
    }

    function duration(seconds) {
        const h = Math.floor(seconds / 3600), m = Math.floor(seconds % 3600 / 60)
        return h > 0 ? qsTr("%1 h %2 min").arg(h.toLocaleString(Qt.locale(), 'f', 0)).arg(m.toLocaleString(Qt.locale(), 'f', 0))
                     : qsTr("%1 min").arg(m.toLocaleString(Qt.locale(), 'f', 0))
    }

    function selectedIndex() {
        const jobs = backend.jobs
        for (let i = 0; i < jobs.length; ++i)
            if (jobs[i].id === selectedId)
                return i
        return -1
    }

    function move(step) {
        const jobs = backend.jobs
        if (jobs.length === 0)
            return
        const i = Math.max(0, Math.min(jobs.length - 1, selectedIndex() + step))
        selectedId = jobs[i].id
        list.positionViewAtIndex(i, ListView.Contain)
    }

    // "?" arrives as Shift+? on most layouts, so bind both.
    Shortcut { sequences: ["?", "Shift+?"]; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: backend.check() }
    Shortcut { sequences: ["J", "Down"]; context: Qt.ApplicationShortcut; onActivated: win.move(1) }
    Shortcut { sequences: ["K", "Up"]; context: Qt.ApplicationShortcut; onActivated: win.move(-1) }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; onActivated: if (win.selectedId) backend.follow(win.selectedId) }
    Shortcut { sequences: ["Delete", "X"]; context: Qt.ApplicationShortcut; onActivated: if (win.selectedId) backend.remove(win.selectedId) }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; context: Qt.ApplicationShortcut; onActivated: backend.redo() }

    Connections {
        target: backend
        // Keep the selection on the same job when the list reloads, or on the
        // first one when that job went away.
        function onJobsChanged() {
            if (win.selectedIndex() < 0)
                win.selectedId = backend.jobs.length > 0 ? backend.jobs[0].id : ""
        }
    }
    Component.onCompleted: if (backend.jobs.length > 0) selectedId = backend.jobs[0].id

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: "Deadbeat"
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }

                AwsBadge {
                    visible: backend.aws.available === true
                    session: backend.aws
                    label: backend.aws.active ? qsTr("AWS: %1 left").arg(win.duration(backend.aws.secondsLeft))
                                              : qsTr("AWS session ended, run aws login")
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: backend.checking
                    visible: running
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                }
                Label {
                    opacity: 0.6
                    text: backend.checking ? qsTr("Checking…")
                          : backend.lastChecked ? qsTr("Checked %1").arg(win.ago(backend.lastChecked))
                          : qsTr("Not checked yet")
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: list
                    anchors.fill: parent
                    clip: true
                    spacing: 8
                    model: backend.jobs
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: JobRow {
                        required property var modelData
                        width: ListView.view.width
                        job: modelData
                        selected: modelData.id === win.selectedId
                        stateText: win.stateLabel(modelData.state)
                        stateColor: win.stateColor(modelData.state)
                        activityText: modelData.activity ? win.ago(modelData.activity) : ""
                        onClicked: win.selectedId = modelData.id
                        onActivated: backend.follow(modelData.id)
                    }
                }

                Column {
                    visible: list.count === 0
                    anchors.centerIn: parent
                    width: Math.min(parent.width, 460)
                    spacing: 8

                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Nothing to watch yet")
                        font.pixelSize: 18
                    }
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        opacity: 0.6
                        text: qsTr("Start a job with deadbeat run, or watch one with deadbeat add. Agents can do both.")
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                opacity: 0.45
                elide: Text.ElideRight
                text: qsTr("Space checks now · Enter follows the log · ? shows every shortcut")
            }
        }

        Rectangle {
            visible: backend.message !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 48
            width: Math.min(parent.width - 40, toast.implicitWidth + 32)
            height: toast.implicitHeight + 20
            radius: Math.min(4, theme.radius)
            color: "#26262c"
            Label {
                id: toast
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 32)
                wrapMode: Text.WordWrap
                text: backend.message
            }
            Timer {
                running: parent.visible
                interval: 5000
                onTriggered: backend.clearMessage()
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onClosed: win.helpVisible = false
        }
    }

    function stateLabel(state) {
        translator.language
        switch (state) {
        case "running": return qsTr("Running")
        case "stalled": return qsTr("Stalled")
        case "done": return qsTr("Done")
        case "failed": return qsTr("Failed")
        case "ended": return qsTr("Ended")
        default: return qsTr("Unknown")
        }
    }

    function stateColor(state) {
        switch (state) {
        // Fixed colours, not the accent: a yellow accent would make running
        // and stalled look the same, and that difference is the point.
        case "running": return "#5aa9f6"
        case "stalled": return "#f5a524"
        case "done": return "#3ccf7a"
        case "failed": return "#f0524f"
        default: return "#7a7a85"
        }
    }
}
