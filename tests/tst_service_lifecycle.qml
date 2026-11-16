import QtQuick
import QtTest
import Quickshell
import "../plugin" as Plugin

// Test the real service with inert process/socket types. No daemon, installer,
// or connection to the user's microphone is started by this suite.
TestCase {
  name: "ServiceLifecycle"
  property var service
  property var daemon
  property var probe
  property var installer

  Component {
    id: serviceComponent
    Plugin.Service {}
  }

  function processWithArgument(argument) {
    for (var i = 0; i < service.children.length; i++) {
      var child = service.children[i]
      if (child.command && child.command.indexOf(argument) !== -1) return child
    }
    fail("Missing process: " + argument)
  }

  function init() {
    Quickshell.socketAvailable = true
    service = createTemporaryObject(serviceComponent, this)
    verify(service)
    daemon = processWithArgument("run")
    probe = processWithArgument("test")
    installer = processWithArgument("mic-effects-install")
    verify(service.connected)
    service.sock.parser.read(JSON.stringify({
      type: "state", mic: { settings: { enabled: true, voiceIsolation: true } }
    }))
  }

  function disconnect() {
    Quickshell.socketAvailable = false
    service.sock.connected = false
    verify(!service.connected)
  }

  function test_startupReusesConnectedDaemon() {
    probe.exited(0)
    wait(650)
    compare(daemon.starts, 0)
    compare(service.settings.voiceIsolation, true)
  }

  function test_duplicateExitPreservesConnectedState() {
    service.installed = true
    daemon.exited(3)
    compare(service.settings.voiceIsolation, true)
    wait(3200)
    compare(daemon.starts, 0)
    compare(service.settings.voiceIsolation, true)
  }

  function test_disconnectedDaemonStartsAndRecoversAfterCrash() {
    disconnect()
    probe.exited(0)
    tryCompare(daemon, "starts", 1, 1200)
    daemon.running = false
    daemon.exited(1)
    tryCompare(daemon, "starts", 2, 2500)
  }

  function test_adoptedDaemonDisconnectStartsReplacement() {
    probe.exited(0)
    wait(650)
    compare(daemon.starts, 0)
    disconnect()
    tryCompare(daemon, "starts", 1, 1200)
  }

  function test_connectionCancelsPendingRetry() {
    disconnect()
    service.installed = true
    daemon.exited(3)
    Quickshell.socketAvailable = true
    service.connectSocket()
    verify(service.connected)
    wait(3200)
    compare(daemon.starts, 0)
  }

  function test_exitBeforeSocketDisconnectStillRecovers() {
    service.installed = true
    daemon.running = true
    daemon.running = false
    daemon.exited(1)
    compare(service.settings.voiceIsolation, true)
    disconnect()
    tryCompare(daemon, "starts", 2, 1200)
  }

  function test_rebuildRestartsOwnedDaemon() {
    daemon.running = true
    installer.exited(0)
    tryCompare(daemon, "signals", 1, 1200)
  }
}
