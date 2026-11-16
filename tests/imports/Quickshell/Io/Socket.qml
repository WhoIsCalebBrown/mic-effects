import QtQuick
import Quickshell

Item {
  property string path: ""
  property bool connected: false
  property var parser: null
  signal connectionStateChanged()
  signal error(string message)

  onConnectedChanged: {
    if (connected && !Quickshell.socketAvailable) connected = false
    connectionStateChanged()
  }
  function write(message) {}
  function flush() {}
}
