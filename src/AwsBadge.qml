import QtQuick
import QtQuick.Controls

// How long the aws login session has left. SSM hosts and every AWS check stop
// working when it ends, so it sits next to the title.
Rectangle {
    id: badge

    property var session: ({})
    property string label

    readonly property bool warning: !session.active || session.secondsLeft < 45 * 60
    readonly property color tint: !session.active ? "#f0524f" : warning ? "#f5a524" : "#7a7a85"

    implicitWidth: text.implicitWidth + 20
    implicitHeight: text.implicitHeight + 8
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(tint.r, tint.g, tint.b, 0.16)
    border.width: 1
    border.color: Qt.rgba(tint.r, tint.g, tint.b, 0.5)

    Label {
        id: text
        anchors.centerIn: parent
        text: badge.label
        color: badge.warning ? badge.tint : "#c8c8d0"
        font.pixelSize: 12
    }
}
