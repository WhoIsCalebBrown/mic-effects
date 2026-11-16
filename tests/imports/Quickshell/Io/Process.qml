import QtQuick

Item {
  property var command: []
  property bool running: false
  property var stderr: null
  property int starts: 0
  property int signals: 0
  signal exited(int code)

  onRunningChanged: if (running) starts++
  function signal(number) { signals++ }
}
