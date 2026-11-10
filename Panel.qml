import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// Deadbeat's bar icon and panel. It only reads what the deadbeat binary
// writes: jobs.json for the jobs, and `deadbeat check` when asked to probe
// now. The binary's own timer does the regular checking and notifying, so
// the shell never runs ssh, aws or gh itself.
Panel {
  id: root
  moduleName: "io.github.themridul.deadbeat"
  ipcTarget: "deadbeat"

  readonly property string stateDir: {
    var custom = Quickshell.env("DEADBEAT_STATE_DIR")
    if (custom) return custom
    var state = Quickshell.env("XDG_STATE_HOME") || (Quickshell.env("HOME") + "/.local/state")
    return state + "/deadbeat"
  }
  readonly property bool hideWhenIdle: setting("hideWhenIdle", true) === true
  readonly property int maxRows: 10

  property var jobs: []
  property bool installed: true
  property var aws: null
  property int now: 0
  property int selected: 0
  property bool cursorActive: false
  property bool enterPressed: false
  property string message: ""
  property string pendingId: ""
  property string pendingName: ""

  readonly property var summary: Model.summary(jobs)
  readonly property var rows: Model.sorted(jobs.filter(function(job) { return job.id !== root.pendingId }))
  readonly property bool checking: checkProc.running
  readonly property bool shown: !hideWhenIdle || jobs.length > 0 || !installed

  function tick() { now = Math.floor(Date.now() / 1000) }

  function reload() { jobsFile.reload() }

  function check() {
    if (!installed || checkProc.running) return
    message = ""
    checkProc.running = true
  }

  function agoText(then) {
    var a = Model.ago(then, now)
    if (!a) return ""
    return a.count === 0 ? strings.t(a.key) : strings.tn(a.key, a.count).arg(Commons.I18n.formatNumber(a.count))
  }

  function statusText() {
    if (!installed) return strings.t("status.notInstalled")
    var parts = []
    var keys = ["stalled", "failed", "running"]
    for (var i = 0; i < keys.length; i++) {
      var n = summary[keys[i]]
      if (n > 0) parts.push(strings.tn("status." + keys[i], n).arg(Commons.I18n.formatNumber(n)))
    }
    return parts.length > 0 ? parts.join(" · ") : strings.t("status.quiet")
  }

  function awsText() {
    if (!aws || aws.error !== "") return ""
    if (!aws.active) return strings.t("aws.ended")
    var minutes = Math.floor(aws.secondsLeft / 60)
    if (minutes < 60) return strings.t("aws.minutes").arg(Commons.I18n.formatNumber(minutes))
    return strings.t("aws.left").arg(Commons.I18n.formatNumber(Math.floor(minutes / 60)))
      .arg(Commons.I18n.formatNumber(minutes % 60))
  }

  function stateColor(state) {
    if (state === "stalled" || state === "failed") return Commons.Color.urgent
    if (state === "done") return Commons.Color.accent
    if (state === "running") return root.bar ? root.bar.foreground : Commons.Color.foreground
    return Commons.Color.muted
  }

  function select(delta) {
    if (rows.length === 0) return
    if (!cursorActive) { cursorActive = true; return }
    selected = Math.max(0, Math.min(Math.min(rows.length, maxRows) - 1, selected + delta))
  }

  function follow(job) {
    var command = job ? Model.followCommand(job) : null
    if (!command) { message = strings.t("cannotOpen"); return }
    Quickshell.execDetached(command)
    close()
  }

  // Removal waits a few seconds so U can take it back; only then does the
  // binary drop the job from jobs.json.
  function remove(job) {
    if (!job) return
    commitRemoval()
    pendingId = job.id
    pendingName = job.name
    removeTimer.restart()
  }

  function commitRemoval() {
    if (pendingId === "") return
    removeTimer.stop()
    Quickshell.execDetached(["deadbeat", "remove", pendingId])
    pendingId = ""
  }

  function undo() {
    removeTimer.stop()
    pendingId = ""
  }

  onOpenedChanged: {
    if (opened) {
      tick()
      reload()
      installedProc.running = true
      awsProc.running = true
      cursorActive = false
      selected = 0
      message = ""
    } else {
      commitRemoval()
    }
  }

  visible: shown
  implicitWidth: shown ? button.implicitWidth : 0
  implicitHeight: shown ? button.implicitHeight : 0

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  FileView {
    id: jobsFile
    path: root.stateDir + "/jobs.json"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.jobs = Model.parseJobs(text())
    onLoadFailed: root.jobs = []
  }

  Process {
    id: installedProc
    command: ["sh", "-c", "command -v deadbeat >/dev/null"]
    onExited: function(code) { root.installed = code === 0 }
  }

  Process {
    id: checkProc
    command: ["deadbeat", "check", "--json"]
    onExited: root.reload()
  }

  Process {
    id: awsProc
    command: ["sh", "-c", "command -v aws-session >/dev/null && aws-session --json"]
    stdout: StdioCollector { waitForEnd: true; onStreamFinished: root.aws = Model.parseAws(text) }
  }

  // jobs.json is replaced on every save, which a file watch can miss, so a
  // cheap re-read backs it up. The clock keeps "5 min ago" honest.
  Timer {
    interval: root.opened ? 5000 : 30000
    running: true
    repeat: true
    triggeredOnStart: true
    onTriggered: { root.tick(); root.reload() }
  }

  Timer { interval: 60000; running: true; repeat: true; triggeredOnStart: true; onTriggered: installedProc.running = true }

  Timer { interval: 60000; running: root.opened; repeat: true; onTriggered: awsProc.running = true }

  Timer { id: removeTimer; interval: 4000; onTriggered: root.commitRemoval() }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    // nf-md-heart_pulse
    text: "󰗶"
    labelText: vertical ? "" : root.summary.attention > 0 ? Commons.I18n.formatNumber(root.summary.attention)
      : root.summary.running > 0 ? Commons.I18n.formatNumber(root.summary.running) : ""
    slotSize: Style.bar.iconSlot * (labelText !== "" ? 1.8 : 1)
    active: root.summary.attention > 0
    dimmed: !root.installed || (root.summary.running === 0 && root.summary.attention === 0)
    tooltipText: ""
    onPressed: function(b) {
      if (b === Qt.MiddleButton) root.check()
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(420))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight)

    content: PanelKeyCatcher {
      anchors.fill: parent
      implicitHeight: column.implicitHeight
      onMoveRequested: function(dx, dy) { if (dy !== 0) root.select(dy) }
      onReturnRequested: root.enterPressed = true
      onActivateRequested: {
        var enter = root.enterPressed
        root.enterPressed = false
        if (!enter) root.check()
        else if (root.cursorActive) root.follow(root.rows[root.selected])
      }
      onDeleteRequested: if (root.cursorActive) root.remove(root.rows[root.selected])
      onTextKey: function(text) { if (text === "u" || text === "U") root.undo() }
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(12)

        // ---------- Hero: icon · name and status · AWS badge ----------
        Item {
          width: parent.width
          implicitHeight: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight)

          Label {
            id: heroIcon
            text: "󰗶"
            color: root.summary.attention > 0 ? Commons.Color.urgent : root.bar.foreground
            font.pixelSize: Style.font.display
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
          }

          Column {
            id: heroLabels
            anchors.left: heroIcon.right
            anchors.leftMargin: Style.space(12)
            anchors.right: checkButton.left
            anchors.rightMargin: Style.space(8)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Label {
              width: parent.width
              text: "Deadbeat"
              font.pixelSize: Style.font.title
              font.bold: true
            }

            Label {
              width: parent.width
              text: root.statusText().toUpperCase()
              color: Qt.darker(root.bar.foreground, 1.4)
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
            }

            Label {
              width: parent.width
              visible: text !== ""
              text: root.awsText()
              color: root.aws && (!root.aws.active || root.aws.secondsLeft < 2700) ? Commons.Color.urgent : Commons.Color.muted
              font.pixelSize: Style.font.caption
            }
          }

          PanelActionButton {
            id: checkButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            // nf-md-refresh
            iconText: "󰑐"
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
            visible: root.installed
            enabled: !root.checking
            opacity: root.checking ? 0.4 : 1
            onClicked: root.check()
          }
        }

        PanelSeparator { foreground: root.bar.foreground }

        // ---------- Not installed ----------
        Column {
          width: parent.width
          visible: !root.installed
          spacing: Style.space(6)

          Label { width: parent.width; wrapMode: Text.Wrap; text: strings.t("install") }
          Label { width: parent.width; text: "yay -S deadbeat"; color: Commons.Color.accent }
        }

        // ---------- Nothing to watch ----------
        Label {
          width: parent.width
          visible: root.installed && root.rows.length === 0 && root.pendingId === ""
          wrapMode: Text.Wrap
          color: Commons.Color.muted
          text: strings.t("empty")
        }

        // ---------- Jobs ----------
        Column {
          width: parent.width
          visible: root.rows.length > 0
          spacing: Style.space(4)

          Repeater {
            model: root.rows.slice(0, root.maxRows)

            delegate: Rectangle {
              id: row
              required property var modelData
              required property int index
              readonly property bool current: root.cursorActive && root.selected === index

              width: parent.width
              implicitHeight: rowContent.implicitHeight + Style.space(12)
              radius: Style.space(6)
              color: current || mouse.containsMouse
                ? Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.08) : "transparent"
              border.width: current ? 1 : 0
              border.color: Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.25)

              Rectangle {
                id: dot
                width: Style.space(8)
                height: width
                radius: width / 2
                color: root.stateColor(row.modelData.state)
                anchors.left: parent.left
                anchors.leftMargin: Style.space(10)
                anchors.top: parent.top
                anchors.topMargin: Style.space(6) + (nameLine.implicitHeight - height) / 2
              }

              Column {
                id: rowContent
                anchors.left: dot.right
                anchors.leftMargin: Style.space(10)
                anchors.right: parent.right
                anchors.rightMargin: Style.space(10)
                anchors.top: parent.top
                anchors.topMargin: Style.space(6)
                spacing: Style.space(2)

                Item {
                  width: parent.width
                  implicitHeight: nameLine.implicitHeight

                  Label {
                    id: nameLine
                    anchors.left: parent.left
                    anchors.right: stateLine.left
                    anchors.rightMargin: Style.space(8)
                    text: row.modelData.name
                    font.bold: true
                  }

                  Label {
                    id: stateLine
                    anchors.right: parent.right
                    text: strings.t("state." + row.modelData.state)
                    color: root.stateColor(row.modelData.state)
                    font.pixelSize: Style.font.bodySmall
                    font.bold: true
                  }
                }

                Label {
                  width: parent.width
                  visible: text !== ""
                  text: row.modelData.detail
                  opacity: 0.85
                  font.pixelSize: Style.font.bodySmall
                }

                // The time stays whole; a long command gives way to it.
                Item {
                  width: parent.width
                  implicitHeight: sourceLine.implicitHeight

                  Label {
                    id: sourceLine
                    anchors.left: parent.left
                    anchors.right: agoLine.left
                    anchors.rightMargin: Style.space(8)
                    text: row.modelData.source
                    color: Commons.Color.muted
                    font.pixelSize: Style.font.caption
                  }

                  Label {
                    id: agoLine
                    anchors.right: parent.right
                    text: root.agoText(row.modelData.activity)
                    color: Commons.Color.muted
                    font.pixelSize: Style.font.caption
                  }
                }
              }

              MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onEntered: { root.cursorActive = true; root.selected = row.index }
                onClicked: root.follow(row.modelData)
              }
            }
          }

          Label {
            visible: root.rows.length > root.maxRows
            text: strings.tn("more", root.rows.length - root.maxRows)
              .arg(Commons.I18n.formatNumber(root.rows.length - root.maxRows))
            color: Commons.Color.muted
            font.pixelSize: Style.font.caption
          }
        }

        // ---------- Undo a removal ----------
        Item {
          width: parent.width
          visible: root.pendingId !== ""
          implicitHeight: removedLine.implicitHeight

          Label {
            id: removedLine
            anchors.left: parent.left
            anchors.right: undoLink.left
            anchors.rightMargin: Style.space(8)
            text: strings.t("removed").arg(root.pendingName)
            font.pixelSize: Style.font.bodySmall
          }

          Label {
            id: undoLink
            anchors.right: parent.right
            text: strings.t("undo")
            color: Commons.Color.accent
            font.pixelSize: Style.font.bodySmall
            font.bold: true

            MouseArea {
              anchors.fill: parent
              cursorShape: Qt.PointingHandCursor
              onClicked: root.undo()
            }
          }
        }

        Label {
          width: parent.width
          visible: root.message !== ""
          text: root.message
          color: Commons.Color.urgent
          font.pixelSize: Style.font.bodySmall
        }

        // ---------- Footer: last check · keys ----------
        Label {
          width: parent.width
          visible: root.installed
          text: (root.checking ? strings.t("checking")
                 : root.summary.checked > 0 ? strings.t("checked").arg(root.agoText(root.summary.checked))
                 : strings.t("notChecked")) + "  ·  " + strings.t("hint")
          color: Commons.Color.muted
          font.pixelSize: Style.font.caption
          wrapMode: Text.Wrap
          elide: Text.ElideNone
        }
      }
    }
  }

  // Every string the panel shows can come from jobs.json, so all text is
  // plain: markup in a job name or log line is shown, never rendered.
  component Label: Text {
    textFormat: Text.PlainText
    color: root.bar ? root.bar.foreground : Commons.Color.foreground
    font.family: root.bar ? root.bar.fontFamily : Style.font.family
    font.pixelSize: Style.font.body
    elide: Text.ElideRight
  }
}
