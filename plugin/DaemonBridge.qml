import QtQuick
import Quickshell
import Quickshell.Io

// Socket client that mirrors Service.qml's panel-facing API.
// Used when a third-party bar (e.g. Islands) cannot resolve serviceFor().
// The keepLoaded Service still owns install/daemon lifecycle; this only talks
// to the already-running ctl.sock (and nudges the binary if the socket is down).
Item {
  id: root

  // Not Item.enabled — that would disable input for children we do not have.
  property bool bridgeActive: true

  property var state: ({})
  property string commandError: ""
  property string setupError: ""
  readonly property bool connected: bridgeActive && sockConnected
  readonly property var mic: state.mic || ({})
  readonly property var settings: mic.settings || ({})
  readonly property var sources: mic.sources || []
  readonly property var source: mic.source || ({})
  readonly property string wantedSource: mic.wanted || ""
  readonly property string outputLabel: mic.label || "Microphone Effects"
  readonly property bool muted: !!mic.muted
  readonly property bool listen: !!mic.listen
  readonly property bool listening: !!mic.listening
  readonly property bool active: !!mic.active
  readonly property int consumers: mic.consumers || 0
  readonly property bool capturing: !!mic.capturing
  readonly property bool hideAll: !!mic.hideAll
  readonly property bool sameForAll: mic.sameForAll !== false
  readonly property real inLevel: mic.inLevel || 0
  readonly property real outLevel: mic.outLevel || 0
  readonly property int rate: mic.rate || 48000
  readonly property string status: mic.status || ""
  readonly property var stageOptions: mic.stages || ["hpf","hum","nr","gate","comp","deess","eq","pitch","fx","verb","limit"]
  readonly property var eqTypeOptions: mic.eqTypes || ["bell","lowshelf","highshelf","highpass","lowpass","notch"]
  readonly property var voiceOptions: mic.voices || ["none","ringmod","megaphone"]
  readonly property var spaceOptions: mic.spaces || ["none","room","hall","cathedral","echo","underwater"]
  readonly property var tuneKeyOptions: mic.tuneKeys || []
  readonly property var tuneScaleOptions: mic.tuneScales || []
  readonly property var userPresets: mic.userPresets || []
  readonly property var chain: {
    var current = settings.chain
    return (current && current.length) ? current : stageOptions
  }

  function send(obj) {
    if (!sockConnected) return false
    commandError = ""
    sock.write(JSON.stringify(obj) + "\n")
    sock.flush()
    return true
  }
  function setMic(patch) { return send({ cmd: "set", mic: patch }) }
  function setSetting(key, value) { var patch = {}; patch[key] = value; return setMic({ settings: patch }) }
  function setSettings(patch) { return setMic({ settings: patch }) }
  function selectSource(nodeName) { return setMic({ source: nodeName }) }
  function setOutputLabel(label) { return setMic({ label: label }) }
  function setMuted(value) { return setMic({ muted: !!value }) }
  function toggleMuted() { return setMuted(!muted) }
  function setListen(value) { return setMic({ listen: !!value }) }
  function setSameForAll(value) { return setMic({ sameForAll: !!value }) }
  function reset() { return send({ cmd: "micreset" }) }
  function refresh() { return send({ cmd: "get" }) }
  function saveUserPreset(key, name) { return send({ cmd: "presetSave", key: key, name: name }) }
  function renameUserPreset(key, name) { return send({ cmd: "presetRename", key: key, name: name }) }
  function deleteUserPreset(key) { return send({ cmd: "presetDelete", key: key }) }

  function moveStage(stageId, delta) {
    var current = chain.slice()
    var from = current.indexOf(stageId)
    if (from < 0) return false
    var to = Math.max(0, Math.min(current.length - 1, from + delta))
    if (from === to) return false
    current.splice(from, 1)
    current.splice(to, 0, stageId)
    return setSetting("chain", current)
  }

  property var meterHolders: ({})
  property bool meterWanted: false
  function setMeter(on, who) {
    var key = who === undefined ? "panel" : String(who)
    var holders = meterHolders
    if (on) holders[key] = true; else delete holders[key]
    meterHolders = holders
    var wanted = false
    for (var name in holders) { wanted = true; break }
    if (wanted === meterWanted) return true
    meterWanted = wanted
    return send({ cmd: "micpreview", on: meterWanted })
  }
  onActiveChanged: setMeter(active, "consumer")

  readonly property string homeDir: Quickshell.env("HOME") || ""
  readonly property string runtimeDir: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/mic-effects"
  readonly property string socketPath: runtimeDir + "/ctl.sock"
  readonly property string libDir: homeDir + "/.local/lib/mic-effects"
  readonly property string daemonBinary: libDir + "/mic-effects-server"

  property var sock: null
  readonly property bool sockConnected: sock ? sock.connected === true : false

  Component {
    id: socketComponent
    Socket {
      path: root.socketPath
      connected: true
      parser: SplitParser {
        onRead: function(line) {
          try {
            var message = JSON.parse(line)
            if (message && message.type === "state") {
              root.state = message
              root.setMeter(!!(message.mic && message.mic.active), "consumer")
            } else if (message && message.type === "error") {
              root.commandError = String(message.error || "Command failed")
            }
          } catch (error) {}
        }
      }
      onConnectionStateChanged: {
        root.sockConnectedChanged()
        if (connected && root.meterWanted) root.send({ cmd: "micpreview", on: true })
        if (!connected) { root.state = ({}); reconnect.restart() }
      }
      onError: function(error) { reconnect.restart() }
    }
  }

  function connectSocket() {
    if (!root.bridgeActive) return
    if (sock) { sock.destroy(); sock = null }
    sock = socketComponent.createObject(root)
  }

  function disconnectSocket() {
    if (sock) { sock.destroy(); sock = null }
    root.state = ({})
  }

  onBridgeActiveChanged: {
    if (bridgeActive) connectSocket()
    else disconnectSocket()
  }

  Timer {
    id: reconnect
    interval: 800
    repeat: false
    onTriggered: if (root.bridgeActive && !root.sockConnected) root.connectSocket()
  }
  Timer {
    interval: 3000
    repeat: true
    running: root.bridgeActive && !root.sockConnected
    onTriggered: root.connectSocket()
  }

  // If the keepLoaded service has not brought the daemon up yet, nudge it.
  // A second instance exits immediately when one is already running.
  Process {
    id: nudge
    command: [root.daemonBinary, "run"]
    running: false
  }
  Process {
    id: probe
    command: ["test", "-x", root.daemonBinary]
    running: false
    onExited: function(code) {
      if (code !== 0)
        root.setupError = "Microphone Effects runtime missing. Open the plugin once under the stock bar, or run install.sh."
      else
        root.setupError = ""
    }
  }
  Timer {
    interval: 2500
    repeat: true
    running: root.bridgeActive && !root.sockConnected
    onTriggered: {
      probe.running = true
      if (!nudge.running) nudge.running = true
    }
  }

  Component.onCompleted: if (bridgeActive) {
    probe.running = true
    connectSocket()
  }
}
