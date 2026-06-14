// SPDX-License-Identifier: Apache-2.0
//
// The whole UI for the coroutine demo. Everything binds to FetchBackend's
// Q_PROPERTYs, which a linear C++ coroutine updates as it runs. The
// BusyIndicator is *always* running: while the three ~0.7s fetches execute,
// it keeps spinning and the window stays draggable — proof the GUI thread is
// never blocked. The same flow with nested QFutureWatcher callbacks would be
// a pyramid of handlers, and Cancel would mean unwinding it by hand; here the
// driving code (FetchBackend::RunSequence) reads top-to-bottom like a script.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CoroDemo

ApplicationWindow {
    id: window
    width: 460
    height: 320
    visible: true
    title: qsTr("C++ Coroutines × Qt/QML")

    FetchBackend {
        id: backend
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // Always spinning: visible evidence the GUI thread stays free
            // while the coroutine awaits each fetch.
            BusyIndicator {
                running: true
                implicitWidth: 32
                implicitHeight: 32
            }

            Label {
                Layout.fillWidth: true
                text: backend.status
                font.pixelSize: 18
                elide: Text.ElideRight
            }
        }

        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: backend.stepCount
            value: backend.step
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                text: qsTr("Start")
                enabled: !backend.busy
                onClicked: backend.start()
            }

            Button {
                text: qsTr("Cancel")
                enabled: backend.busy
                onClicked: backend.cancel()
            }

            Item {
                Layout.fillWidth: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#11000000"
            radius: 6

            Label {
                anchors.fill: parent
                anchors.margins: 10
                text: backend.result
                wrapMode: Text.Wrap
                verticalAlignment: Text.AlignTop
                font.family: Qt.application.font.family
            }
        }
    }
}
