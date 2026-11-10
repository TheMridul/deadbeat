import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, shown with ?. Click anywhere or press Escape to close.
Rectangle {
    id: overlay
    signal closed()
    color: "#cc0e0e10"

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.closed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, grid.implicitWidth + 56)
        height: column.implicitHeight + 48
        radius: theme.radius
        color: "#17171c"
        border.width: 1
        border.color: "#2a2a31"

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 24
            spacing: 16

            Label {
                text: qsTr("Keyboard shortcuts")
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }

            GridLayout {
                id: grid
                columns: 2
                columnSpacing: 24
                rowSpacing: 10

                // Keys stay as they are in every language.
                Repeater {
                    model: ["Space", "J / K", "Enter", "X / Delete", "Ctrl+Z", "Ctrl+Shift+Z", "?", "Q"]
                    delegate: Label {
                        required property var modelData
                        required property int index
                        Layout.row: index
                        Layout.column: 0
                        text: modelData
                        color: theme.accent
                        font.family: "monospace"
                    }
                }
                Repeater {
                    model: [
                        qsTr("Check every job now"),
                        qsTr("Select the next or previous job"),
                        qsTr("Follow the selected job's log"),
                        qsTr("Stop watching the selected job"),
                        qsTr("Undo"),
                        qsTr("Redo"),
                        qsTr("Show or hide this list"),
                        qsTr("Quit")
                    ]
                    delegate: Label {
                        required property var modelData
                        required property int index
                        Layout.row: index
                        Layout.column: 1
                        text: modelData
                    }
                }
            }
        }
    }
}
