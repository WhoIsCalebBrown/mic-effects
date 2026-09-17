import QtQuick
import Quickshell
import Quickshell.Io

// Owns the local PipeWire/DSP daemon and mirrors its microphone state.
Item {
  id: root
  property var shell: null
  property var manifest: null

  property var state: ({})
  readonly property bool connected: sockConnected
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
  readonly property var stageOptions: mic.stages || ["hpf","hum","nr","gate","comp","deess","eq","pitch","fx","verb"]
  readonly property var eqTypeOptions: mic.eqTypes || ["bell","lowshelf","highshelf","highpass","lowpass","notch"]
  readonly property var voiceOptions: mic.voices || ["none","ringmod","megaphone"]
  readonly property var spaceOptions: mic.spaces || ["none","room","hall","cathedral","echo","underwater"]
  readonly property var tuneKeyOptions: mic.tuneKeys || []
  readonly property var tuneScaleOptions: mic.tuneScales || []
  readonly property var chain: {
    var current = settings.chain
    return (current && current.length) ? current : stageOptions
  }

  function send(obj) {
    if (!sockConnected) return false
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
  readonly property string repoDir: decodeURIComponent(String(Qt.resolvedUrl("..")).replace(/^file:\/\//, "").replace(/\/$/, ""))
  readonly property string cacheDir: (Quickshell.env("XDG_CACHE_HOME") || (homeDir + "/.cache")) + "/mic-effects"
  readonly property string installLog: cacheDir + "/install.log"
  property string daemonLog: ""
  property string setupError: ""
  property bool installed: false
  property int restarts: 0

  Process {
    id: probe
    command: ["test", "-x", root.daemonBinary]
    onExited: function(code) {
      root.installed = code === 0
      if (root.installed) restartTimer.restart()
      else install.running = true
    }
  }
  Process {
    id: install
    command: ["sh", "-c", 'mkdir -p "$2"; cd "$1" && ./install.sh >"$2/install.log" 2>&1', "mic-effects-install", root.repoDir, root.cacheDir]
    onExited: function(code) {
      if (code === 0) { root.installed = true; root.setupError = ""; restartTimer.restart() }
      else root.setupError = "Microphone Effects build failed. See " + root.installLog
    }
  }
  Process {
    id: updateCheck
    command: ["sh", "-c", 'a=$(git -C "$1" rev-parse HEAD 2>/dev/null) || exit 0; b=$(cat "$2/installed-commit" 2>/dev/null); [ -x "$2/mic-effects-server" ] && [ -n "$a" ] && [ "$a" != "$b" ] && exit 3; exit 0', "mic-effects-update", root.repoDir, root.libDir]
    onExited: function(code) { if (code === 3 && !install.running) install.running = true }
  }

  Process {
    id: daemon
    command: [root.daemonBinary, "run"]
    running: false
    stderr: SplitParser {
      onRead: function(line) {
        var next = root.daemonLog + line + "\n"
        root.daemonLog = next.length > 4000 ? next.slice(next.length - 4000) : next
      }
    }
    onExited: function(code) {
      root.state = ({})
      root.restarts += 1
      restartTimer.interval = code === 3 ? 3000 : Math.min(10000, 800 + root.restarts * 800)
      restartTimer.restart()
    }
  }
  Timer {
    id: restartTimer
    interval: 500
    repeat: false
    onTriggered: {
      if (!root.installed || install.running) return
      if (daemon.running) daemon.signal(15)
      else { daemon.command = [root.daemonBinary, "run"]; daemon.running = true }
    }
  }
  Timer { interval: 60000; running: daemon.running; repeat: false; onTriggered: root.restarts = 0 }

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
    if (sock) { sock.destroy(); sock = null }
    sock = socketComponent.createObject(root)
  }
  Timer { id: reconnect; interval: 800; repeat: false; onTriggered: if (!root.sockConnected) root.connectSocket() }
  Timer { interval: 3000; repeat: true; running: !root.sockConnected; onTriggered: root.connectSocket() }

  Component.onCompleted: {
    probe.running = true
    connectSocket()
    updateCheck.running = true
  }
  Component.onDestruction: if (daemon.running) daemon.signal(15)
}
