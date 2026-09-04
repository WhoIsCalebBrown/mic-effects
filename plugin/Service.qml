import QtQuick
import Quickshell
import Quickshell.Io

// Headless client for the microphone half of camera-effects-server.
//
// It does NOT own the daemon. The camera plugin's service starts it, restarts
// it and holds the camera preview; this one only opens its own control
// connection to the same socket and mirrors the `mic` half of the state it
// pushes. That is why the two plugins can be enabled and disabled
// independently: whichever is up talks to whatever daemon is there, and
// neither has to know the other exists.
Item {
  id: root

  property var shell: null
  property var manifest: null

  // ---- daemon state (the `mic` object of the pushed JSON) ----------------
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
  readonly property bool active: !!mic.active            // an app is holding the virtual mic
  readonly property int consumers: mic.consumers || 0
  readonly property bool capturing: !!mic.capturing      // the real mic is open
  readonly property bool hideAll: !!mic.hideAll
  readonly property bool sameForAll: mic.sameForAll !== false
  readonly property real inLevel: mic.inLevel || 0
  readonly property real outLevel: mic.outLevel || 0
  readonly property int rate: mic.rate || 48000
  readonly property string status: mic.status || ""

  // Vocabularies the daemon owns. Defaults only cover the gap before the first
  // state arrives — the daemon's list always wins once it does.
  readonly property var stageOptions: mic.stages || ["hpf","hum","nr","gate","comp","deess","eq","pitch","fx","verb"]
  readonly property var eqTypeOptions: mic.eqTypes || ["bell","lowshelf","highshelf","highpass","lowpass","notch"]
  readonly property var voiceOptions: mic.voices || ["none","ringmod","megaphone"]
  readonly property var spaceOptions: mic.spaces || ["none","room","hall","cathedral","echo","underwater"]
  readonly property var tuneKeyOptions: mic.tuneKeys || []
  readonly property var tuneScaleOptions: mic.tuneScales || []

  // The chain as a complete order, whatever the daemon happens to hold: empty
  // means "the default", and the panel should never have to know that.
  readonly property var chain: {
    var c = settings.chain
    return (c && c.length) ? c : stageOptions
  }

  // ---- writes ------------------------------------------------------------
  function send(obj) {
    if (!sockConnected) return false
    sock.write(JSON.stringify(obj) + "\n")
    sock.flush()
    return true
  }
  function setMic(patch) { return send({ cmd: "set", mic: patch }) }
  function setSetting(key, value) { var p = {}; p[key] = value; return setMic({ settings: p }) }
  function setSettings(patch) { return setMic({ settings: patch }) }
  function selectSource(nodeName) { return setMic({ source: nodeName }) }
  function setOutputLabel(label) { return setMic({ label: label }) }
  function setMuted(v) { return setMic({ muted: !!v }) }
  function toggleMuted() { return setMuted(!muted) }
  function setListen(v) { return setMic({ listen: !!v }) }
  function setSameForAll(v) { return setMic({ sameForAll: !!v }) }
  function reset() { return send({ cmd: "micreset" }) }
  function refresh() { return send({ cmd: "status" }) }

  // Move one stage of the chain by `delta` places, clamped at the ends.
  function moveStage(id, delta) {
    var c = chain.slice()
    var i = c.indexOf(id)
    if (i < 0) return false
    var j = Math.max(0, Math.min(c.length - 1, i + delta))
    if (i === j) return false
    c.splice(i, 1)
    c.splice(j, 0, id)
    return setSetting("chain", c)
  }

  // The level meter needs the real microphone open, which costs the recording
  // light and a stream. Hold it only while something is actually looking.
  property var meterHolders: ({})
  property bool meterWanted: false
  function setMeter(on, who) {
    var key = who === undefined ? "panel" : String(who)
    var h = meterHolders
    if (on) h[key] = true; else delete h[key]
    meterHolders = h
    var want = false
    for (var k in h) { want = true; break }
    if (want === meterWanted) return true
    meterWanted = want
    return send({ cmd: "micpreview", on: meterWanted })
  }

  // A virtual-mic consumer already keeps the real microphone open. Holding the
  // meter stream in that case is free, and avoids depending solely on a popup
  // lifecycle signal (there is one panel instance per monitor, so that signal
  // is not reliable enough to be the only owner).
  onActiveChanged: setMeter(active, "consumer")

  // ---- connection --------------------------------------------------------
  readonly property string runtimeDir: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/camera-effects"
  readonly property string socketPath: runtimeDir + "/ctl.sock"

  // Quickshell's Socket cannot recover from a refused connection, so every
  // attempt gets a fresh object rather than a retry on a dead one. Same reason
  // the camera service does it this way.
  property var sock: null
  readonly property bool sockConnected: sock ? sock.connected === true : false

  Component {
    id: sockComp
    Socket {
      path: root.socketPath
      connected: true
      parser: SplitParser {
        onRead: function(line) {
          try {
            var msg = JSON.parse(line)
            if (msg && msg.type === "state") {
              root.state = msg
              // State is delivered only after the socket is writable, which
              // makes this the reliable place to acquire the consumer meter
              // holder (rather than relying on binding-signal ordering).
              root.setMeter(!!(msg.mic && msg.mic.active), "consumer")
            }
          } catch (e) { /* a reply we did not ask about */ }
        }
      }
      onConnectionStateChanged: {
        root.sockConnectedChanged()
        if (connected && root.meterWanted) root.send({ cmd: "micpreview", on: true })
        if (!connected) { root.state = ({}); reconnectTimer.restart() }
      }
      onError: function(err) { reconnectTimer.restart() }
    }
  }

  function connectSocket() {
    if (sock) { sock.destroy(); sock = null }
    sock = sockComp.createObject(root)
  }

  Timer {
    id: reconnectTimer
    // The daemon may not be up yet (the camera plugin starts it), so this is a
    // normal state to sit in rather than an error to report.
    interval: 2000
    repeat: false
    onTriggered: if (!root.sockConnected) root.connectSocket()
  }

  Timer {
    interval: 5000
    repeat: true
    running: true
    onTriggered: if (!root.sockConnected) root.connectSocket()
  }

  Component.onCompleted: connectSocket()
}
