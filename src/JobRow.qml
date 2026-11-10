import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One watched job: a state dot, the name and state, the latest line, and
// where it runs. Click selects it; double-click follows its log.
Rectangle {
    id: row

    property var job
    property bool selected: false
    property string stateText
    property color stateColor
    property string activityText

    signal clicked()
    signal activated()

    implicitHeight: content.implicitHeight + 24
    radius: theme.radius
    color: selected ? "#1d1d23" : "#151519"
    border.width: 1
    border.color: selected ? theme.accent : "#24242b"

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 12
        anchors.leftMargin: 16
        spacing: 14

        // A true circle, so it keeps its own radius whatever the theme rounds.
        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 6
            width: 10
            height: 10
            radius: 5
            color: row.stateColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    text: row.job.name
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Label {
                    text: row.stateText
                    color: row.stateColor
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
                Label {
                    visible: text !== ""
                    text: row.activityText
                    opacity: 0.55
                    font.pixelSize: 13
                }
            }
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: row.job.detail
                font.family: "monospace"
                font.pixelSize: 12
                opacity: 0.85
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: row.job.source
                font.pixelSize: 12
                opacity: 0.45
                elide: Text.ElideMiddle
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: row.clicked()
        onDoubleClicked: row.activated()
    }
}
