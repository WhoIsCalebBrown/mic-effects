import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui
import "Presets.js" as Presets

// Bar icon + popup for the microphone half of Camera Effects, drawn as a rack
// of console units rather than a settings list.
//
// The shape of it: the source, the meters and mute/monitor/level are pinned at
// the top and never scroll, because those are what a call needs. Under them a
// rail of stage modules is both the status display (a lit LED means engaged)
// and the selector, and only the stage you pick opens below it. Ten stages and
// their forty-odd controls therefore cost the height of one stage, not ten.
Panel {
  id: root
  moduleName: "caleb.mic-effects"
  ipcTarget: "caleb.mic-effects"

  readonly property var svc: bar && bar.shell ? bar.shell.serviceFor("caleb.mic-effects") : null
  readonly property var m: svc ? svc.settings : ({})
  readonly property bool connected: svc ? svc.connected : false
  readonly property bool muted: svc ? svc.muted : false
  readonly property bool live: svc ? (svc.active && !muted) : false
  readonly property bool alwaysShow: setting("alwaysShow", true)

  // The bar lays widgets out by their implicit size. Without these the item is
  // zero-wide and the icon is allocated no space at all — present, bound, and
  // invisible.
  visible: alwaysShow || muted || live
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  // A click outside dismisses, like every other bar popup.
  function close() { controller.hide() }
  function userClose() { controller.hide() }
  function toggle() { opened ? userClose() : open() }

  // ---- palette -----------------------------------------------------------
  readonly property color fg: bar ? bar.foreground : Color.foreground
  readonly property color dim: Qt.darker(fg, 1.45)
  readonly property color eng: Qt.darker(fg, 1.9)      // engraved caps: quieter than dim
  readonly property color accent: Color.popups.border
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property int rowH: Style.spacing.popupRowHeight
  readonly property int inset: Style.space(11)
  // This is deliberately a scale of the whole rack rather than only a wider
  // popup: audio controls need their original proportions to stay readable.
  readonly property real popupScale: 1.35

  // ---- the chain ---------------------------------------------------------
  readonly property var chain: svc ? svc.chain : []
  property string stage: "comp"          // which unit is open
  readonly property var stageLabels: ({
    hpf: "HPF", hum: "HUM", nr: "NR", gate: "GATE", comp: "COMP",
    deess: "DE-S", eq: "EQ", pitch: "PITCH", fx: "FX", verb: "VERB"
  })
  readonly property var stageTitles: ({
    hpf: "HIGH-PASS FILTER", hum: "HUM NOTCH", nr: "NOISE REDUCTION", gate: "GATE",
    comp: "COMPRESSOR", deess: "DE-ESSER", eq: "EQ", pitch: "PITCH",
    fx: "VOICE FX", verb: "REVERB & DELAY SENDS"
  })
  function stageOn(id) {
    var s = m
    if (!s) return false
    if (id === "hpf") return !!s.highPass
    if (id === "hum") return !!s.humFilter
    if (id === "nr") return !!s.voiceIsolation
    if (id === "gate") return !!s.noiseGate
    if (id === "comp") return !!s.autoLevel
    if (id === "deess") return !!s.deEsser
    if (id === "eq") return !!(s.eq && s.eq.length)
    if (id === "pitch") return !!(s.autoTune || s.pitch || s.formant || s.doubler)
    if (id === "fx") return !!(s.tape || s.ringMod || s.megaphone)
    if (id === "verb") return !!((s.space && s.space !== "none") || s.slapDelay || s.longDelay)
    return false
  }
  readonly property int engagedCount: {
    var n = 0
    for (var i = 0; i < chain.length; i++) if (stageOn(chain[i])) n++
    return n
  }

  // ---- helpers -----------------------------------------------------------
  function db(v) { return (v > 0 ? "+" : "") + v.toFixed(1) }
  function dbfs(v) { return v > 0.00001 ? Math.max(-96, 20 * Math.log(v) / Math.LN10).toFixed(0) + " dB" : "−∞ dB" }
  function semis(v) { return (v > 0 ? "+" : "") + v.toFixed(1) + " st" }
  function hz(f) { return f >= 1000 ? (f / 1000).toFixed(f >= 10000 ? 0 : 2) + "k" : Math.round(f) + "" }
  function freqToPos(f) { return Math.log(Math.max(20, f) / 20) / Math.log(1000) }
  function posToFreq(p) { return 20 * Math.pow(1000, p) }
  // Mirrors of the daemon's own mappings, so a readout is the number the DSP is
  // using rather than a decoration.
  function gateThresholdDb(t) { return -60 + 45 * t }
  function compRatio(t) { return 2 + 4 * t }
  function compThresholdDb(t) { return -14 - 18 * t }
  function retuneMs(t) { return 200 * Math.pow(0.01, t) }
  function num(key, fallback) { var v = m ? m[key] : undefined; return v === undefined ? fallback : v }
  function set(key, value) { if (svc) svc.setSetting(key, value) }
  function sourceLabel(source) {
    if (!source) return "Microphone"
    var label = String(source.description || source.name || "Microphone")
    return label.length > 28 ? label.slice(0, 27) + "…" : label
  }
  readonly property var inputOptions: {
    var options = [{ label: "System Default", value: "" }]
    var sources = svc ? svc.sources : []
    for (var i = 0; i < sources.length; i++)
      options.push({ label: sourceLabel(sources[i]), value: String(sources[i].name) })
    return options
  }
  function inputOptionIndex() {
    var wanted = svc ? svc.wantedSource : ""
    for (var i = 0; i < inputOptions.length; i++) if (inputOptions[i].value === wanted) return i
    return 0
  }
  function setFx(key, on) {
    if (!svc) return
    var patch = { voice: "none" }  // clears the pre-independent legacy selector
    patch[key] = on
    svc.setSettings(patch)
  }
  function spaceLabel(key) {
    if (key === "none") return "Off"
    if (key === "trap") return "Trap legacy"
    return key
  }
  function voiceLabel(key) {
    if (key === "none") return "Off"
    if (key === "tape") return "Tape"
    if (key === "ringmod") return "Ring Mod"
    return "Megaphone"
  }

  // =========================================================================
  // The console kit
  // =========================================================================

  // A rack unit: a band of the panel separated from its neighbours by a cut
  // groove — dark line below, hairline highlight under that — rather than a
  // drawn border. Alpha over whatever the theme's ground is, so it survives a
  // theme change instead of assuming a dark one.
  component RackUnit: Item {
    id: ru
    default property alias content: ruBody.data
    property real pad: Style.space(9)
    property bool last: false
    width: parent ? parent.width : 200
    implicitHeight: ruBody.implicitHeight + pad * 2 + (last ? 0 : 2)
    height: implicitHeight
    Column {
      id: ruBody
      x: root.inset
      y: ru.pad
      width: parent.width - root.inset * 2
      spacing: Style.space(6)
    }
    Rectangle {
      visible: !ru.last
      anchors.bottom: parent.bottom
      anchors.bottomMargin: 1
      width: parent.width
      height: 1
      color: Qt.rgba(0, 0, 0, 0.75)
    }
    Rectangle {
      visible: !ru.last
      anchors.bottom: parent.bottom
      width: parent.width
      height: 1
      color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.055)
    }
  }

  // Engraved caps: the label style the whole rack is titled in.
  component Eng: Text {
    property real tracking: 0.2
    color: root.eng
    font.family: root.fontFamily
    font.pixelSize: Style.font.caption
    // The shell's font metrics resolve caption tracking through this item,
    // so binding letterSpacing here creates a Qt feedback loop. The mono face
    // remains legible without synthetic tracking.
    font.capitalization: Font.AllUppercase
  }

  // An indicator lamp. Off is a recessed dark bead; on carries the accent and a
  // little bloom, which is what makes "what is engaged" readable at a glance.
  component Led: Rectangle {
    property bool on: false
    property color tint: root.accent
    width: Style.space(7); height: width; radius: width / 2
    color: on ? tint : Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.10)
    border.width: 1
    border.color: on ? Qt.lighter(tint, 1.3) : Qt.rgba(0, 0, 0, 0.6)
    Rectangle {   // bloom
      visible: parent.on
      anchors.centerIn: parent
      width: parent.width * 2.2; height: width; radius: width / 2
      color: Qt.rgba(parent.tint.r, parent.tint.g, parent.tint.b, 0.22)
      z: -1
    }
  }

  // A stage in the rail: name, lamp, and the selected one ringed in accent.
  component StageModule: Rectangle {
    id: sm
    property string stageId: ""
    property bool selected: false
    property bool engaged: false
    signal picked()
    implicitWidth: smCol.implicitWidth + Style.space(14)
    implicitHeight: smCol.implicitHeight + Style.space(10)
    color: selected ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
                    : Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.045)
    border.width: 1
    border.color: selected ? root.accent : Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.13)
    Rectangle {   // top hairline: the lit edge of a raised block
      anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
      anchors.margins: 1
      height: 1
      color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.07)
    }
    Column {
      id: smCol
      anchors.centerIn: parent
      spacing: Style.space(3)
      Eng {
        text: root.stageLabels[sm.stageId] || sm.stageId
        tracking: 0.1
        color: sm.selected ? root.fg : root.dim
        anchors.horizontalCenter: parent.horizontalCenter
      }
      Led {
        on: sm.engaged
        anchors.horizontalCenter: parent.horizontalCenter
      }
    }
    MouseArea {
      anchors.fill: parent
      cursorShape: Qt.PointingHandCursor
      onClicked: sm.picked()
    }
  }

  // Segmented level meter. Segments rather than a bar because a console meter
  // reads as a count of lit cells, and because it makes the top of the scale
  // legible without a number next to it.
  component SegMeter: Row {
    id: meter
    property real level: 0
    property int cells: 18
    spacing: 1
    Repeater {
      model: meter.cells
      delegate: Rectangle {
        required property int index
        readonly property real at: (index + 1) / meter.cells
        readonly property bool lit: meter.level >= at
        width: Style.space(6); height: Style.space(9)
        color: !lit ? Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.09)
             : at > 0.88 ? "#de9562"
             : at > 0.66 ? "#c89674"
             : "#e2b969"
        Behavior on color { ColorAnimation { duration: 55 } }
      }
    }
  }

  // ---- presets ----------------------------------------------------------
  readonly property var eqPresets: Presets.eq
  readonly property var micPresets: Presets.mic
  // Space buttons are starting points, not a mode switch. Each one sets the
  // algorithm plus a sensible physical shape, then the knobs stay yours.
  readonly property var reverbPresets: ({
    room:       { spaceSize: 0.30, spaceDecay: 0.30, spaceTone: 0.58, spacePreDelay: 0.02, spaceDiffusion: 0.52, spaceLowCut: 0.08, spaceModRate: 0.30, spaceModDepth: 0.03, spaceMix: 1.0 },
    hall:       { spaceSize: 0.58, spaceDecay: 0.58, spaceTone: 0.50, spacePreDelay: 0.10, spaceDiffusion: 0.65, spaceLowCut: 0.12, spaceModRate: 0.40, spaceModDepth: 0.08, spaceMix: 1.0 },
    cathedral:  { spaceSize: 0.82, spaceDecay: 0.82, spaceTone: 0.40, spacePreDelay: 0.22, spaceDiffusion: 0.78, spaceLowCut: 0.18, spaceModRate: 0.32, spaceModDepth: 0.12, spaceMix: 1.0 },
    echo:       { spaceSize: 0.55, spaceDecay: 0.42, spaceTone: 0.65, spacePreDelay: 0.28, spaceDiffusion: 0.50, spaceLowCut: 0.06, spaceModRate: 0.50, spaceModDepth: 0.00, spaceMix: 1.0 },
    underwater: { spaceSize: 0.45, spaceDecay: 0.60, spaceTone: 0.15, spacePreDelay: 0.06, spaceDiffusion: 0.60, spaceLowCut: 0.20, spaceModRate: 0.35, spaceModDepth: 0.45, spaceMix: 1.0 },
    trap:       { spaceSize: 0.65, spaceDecay: 0.68, spaceTone: 0.58, spacePreDelay: 0.20, spaceDiffusion: 0.72, spaceLowCut: 0.16, spaceModRate: 0.36, spaceModDepth: 0.10, spaceMix: 0.88 }
  })
  property bool showEqPresets: false
  readonly property string eqKey: JSON.stringify(eqBands)
  property string activeEqPreset: "custom"
  onEqKeyChanged: activeEqPreset = matchEqPreset()
  function eqPresetBands(key) {
    for (var i = 0; i < eqPresets.length; i++) if (eqPresets[i].key === key) return eqPresets[i].bands
    return null
  }
  function matchEqPreset() {
    for (var i = 0; i < eqPresets.length; i++) {
      var want = eqPresets[i].bands
      if (want.length !== eqBands.length) continue
      var match = true
      for (var j = 0; j < want.length; j++) {
        var got = eqBands[j]
        if (!got || got.on === false || got.type !== want[j].type
            || Math.abs(got.freq - want[j].freq) > 0.5
            || Math.abs(got.gain - want[j].gain) > 0.05
            || Math.abs(got.q - want[j].q) > 0.005) { match = false; break }
      }
      if (match) return eqPresets[i].key
    }
    return "custom"
  }
  function applyEqPreset(key) {
    var bands = eqPresetBands(key)
    if (!svc || bands === null) return
    var next = []
    for (var i = 0; i < bands.length; i++)
      next.push({ on: true, type: bands[i].type, freq: bands[i].freq, gain: bands[i].gain, q: bands[i].q })
    eqSel = 0
    svc.setSetting("eq", next)
  }
  // Channel presets are complete snapshots of the effects rack.  Starting
  // from this quiet baseline is deliberate: a Meeting preset must not inherit
  // Tape, a doubler, delays, or an old tuning key from a previous Trap setup.
  function micPresetBase() {
    return {
      voiceIsolation: false, voiceIsolationIntensity: 0.6,
      noiseGate: false, noiseGateIntensity: 0.55,
      autoLevel: false, autoLevelIntensity: 0.6, glueComp: false, glueCompIntensity: 0.4,
      deEsser: false, deEsserIntensity: 0.5, highPass: true, humFilter: false,
      chain: [], voice: "none", tape: false, tapeMix: 0.45, ringMod: false, ringModMix: 1.0, megaphone: false, megaphoneMix: 1.0,
      // The song key is a user choice, not a channel-preset choice.  Keep it
      // when changing presets; Tuned Trap below selects a useful scale but
      // never silently changes the root note they chose for the song.
      autoTune: false, autoTuneSpeed: 0.5, autoTuneAmount: 1.0,
      pitch: 0, formant: 0, doubler: false, doublerMix: 0.35, compMix: 1.0, pitchMix: 1.0,
      space: "none", spaceSize: 0.5, spaceDecay: 0.5, spaceTone: 0.5, spacePreDelay: 0, spaceDiffusion: 0.5, spaceLowCut: 0, spaceModRate: 0.5, spaceModDepth: 0, spaceMix: 1.0,
      slapDelay: false, slapDelayMix: 0.18, slapDelayTime: 0.4,
      longDelay: false, longDelayMix: 0.28, longDelayTime: 0.4, longDelayFeedback: 0.32, longDelayTone: 0.45
    }
  }
  function applyMicPreset(key) {
    if (!svc) return
    for (var i = 0; i < micPresets.length; i++) {
      var preset = micPresets[i]
      if (preset.key !== key) continue
      var patch = micPresetBase()
      for (var setting in preset.set) if (setting !== "eqPreset") patch[setting] = preset.set[setting]
      var bands = eqPresetBands(preset.set.eqPreset) || []
      patch.eq = []
      for (var j = 0; j < bands.length; j++)
        patch.eq.push({ on: true, type: bands[j].type, freq: bands[j].freq, gain: bands[j].gain, q: bands[j].q })
      patch.enabled = true
      svc.setSettings(patch)
      eqSel = 0
      return
    }
  }
  function applyReverbPreset(kind) {
    if (!svc) return
    var patch = { space: kind }
    var preset = reverbPresets[kind]
    if (preset) for (var setting in preset) patch[setting] = preset[setting]
    svc.setSettings(patch)
  }

  // A knob. Vertical drag turns it — the gesture every plugin uses — and a
  // double-click puts it back to stock, matching the faders.
  component Knob: Item {
    id: kb
    property string label: ""
    property string readout: ""
    property real value: 0.5           // 0..1 of its travel
    property real defaultValue: NaN
    // The readout is a direct numeric field too.  Most console controls are
    // percentages; controls with real-world units override these ranges below.
    property real inputMin: 0
    property real inputMax: 100
    property int inputDecimals: 0
    property string inputUnit: "%"
    property var valueToInput: null
    property var inputToValue: null
    signal moved(real v)
    signal released(real v)
    property real live: value
    width: Style.space(52)
    implicitHeight: dial.height + kbL.height + kbV.height + Style.space(10)
    function inputNumber() {
      var n = valueToInput ? valueToInput(live) : inputMin + live * (inputMax - inputMin)
      return Number(n).toFixed(inputDecimals)
    }
    function openInput() {
      kbEdit.text = inputNumber()
      kbEdit.forceActiveFocus()
      kbEdit.selectAll()
    }
    function acceptInput() {
      var n = Number(kbEdit.text.trim().replace(/[^0-9.+-]/g, ""))
      if (!isFinite(n)) { kbEdit.focus = false; return }
      var v = inputToValue ? inputToValue(n) : (n - inputMin) / (inputMax - inputMin)
      v = Math.max(0, Math.min(1, v))
      kb.live = v
      kb.moved(v)
      kb.released(v)
      kbEdit.focus = false
    }
    Rectangle {
      id: dial
      anchors.horizontalCenter: parent.horizontalCenter
      width: Style.space(34); height: width; radius: width / 2
      color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.06)
      border.width: 1
      border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, kbArea.containsMouse || kbArea.pressed ? 0.34 : 0.18)
      Rectangle {   // pointer: 135deg of dead zone at the bottom, like a real one
        width: 2
        height: parent.height * 0.34
        radius: 1
        color: root.fg
        x: parent.width / 2 - 1
        y: Style.space(3)
        transformOrigin: Item.Bottom
        rotation: -135 + 270 * Math.max(0, Math.min(1, kb.live))
      }
      MouseArea {
        id: kbArea
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        drag.filterChildren: true
        cursorShape: Qt.SizeVerCursor
        property real startY: 0
        property real startV: 0
        property real lastRelease: 0
        onPressed: function(mouse) { startY = mouse.y; startV = kb.live; kb.forceActiveFocus() }
        onPositionChanged: function(mouse) {
          if (!pressed) return
          // 140 px of travel for the full range: fine enough to place a value,
          // coarse enough to cross it in one gesture.
          var v = Math.max(0, Math.min(1, startV + (startY - mouse.y) / 140))
          kb.live = v
          kb.moved(v)
        }
        onReleased: {
          var now = Date.now()
          if (!isNaN(kb.defaultValue) && now - lastRelease < 350) {
            lastRelease = 0
            kb.live = kb.defaultValue
            kb.released(kb.defaultValue)
            return
          }
          lastRelease = now
          kb.released(kb.live)
        }
      }
    }
    Eng {
      id: kbL
      anchors.top: dial.bottom
      anchors.topMargin: Style.space(4)
      anchors.horizontalCenter: parent.horizontalCenter
      text: kb.label
      tracking: 0.1
    }
    Item {
      id: kbV
      anchors.top: kbL.bottom
      anchors.topMargin: Style.space(1)
      anchors.horizontalCenter: parent.horizontalCenter
      width: Math.max(kb.width, Style.space(54)); height: Style.font.caption + Style.space(4)
      Text {
        anchors.centerIn: parent
        visible: !kbEdit.activeFocus
        text: kb.readout
        color: root.fg
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
      }
      MouseArea {
        anchors.fill: parent
        visible: !kbEdit.activeFocus
        cursorShape: Qt.IBeamCursor
        onClicked: kb.openInput()
      }
      TextInput {
        id: kbEdit
        anchors.fill: parent
        visible: activeFocus
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        color: root.fg
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        selectByMouse: true
        validator: RegularExpressionValidator { regularExpression: /[-+]?\d*\.?\d*/ }
        onEditingFinished: kb.acceptInput()
      }
    }
    onValueChanged: if (!kbArea.pressed) live = value
  }

  // A console fader: a cut channel with a metal cap, for the two values that
  // really are "how much of a range" rather than "what value".
  component Fader: Item {
    id: fd
    property string label: ""
    property string readout: ""
    property real value: 0.5
    property real from: 0
    property real to: 1
    property real defaultValue: NaN
    property real inputMin: from
    property real inputMax: to
    property int inputDecimals: 1
    signal moved(real v)
    signal released(real v)
    property real live: value
    width: parent ? parent.width : 200
    height: root.rowH
    readonly property real frac: (live - from) / Math.max(0.0001, to - from)
    function openInput() {
      fdEdit.text = (inputMin + (live - from) / Math.max(0.0001, to - from) * (inputMax - inputMin)).toFixed(inputDecimals)
      fdEdit.forceActiveFocus()
      fdEdit.selectAll()
    }
    function acceptInput() {
      var n = Number(fdEdit.text.trim().replace(/[^0-9.+-]/g, ""))
      if (isFinite(n)) {
        var f = Math.max(0, Math.min(1, (n - inputMin) / (inputMax - inputMin)))
        fd.live = from + f * (to - from)
        fd.moved(fd.live)
        fd.released(fd.live)
      }
      fdEdit.focus = false
    }
    Eng { id: fdL; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: fd.label; tracking: 0.1 }
    Item {
      id: fdV
      anchors.right: chn.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(52); height: Style.font.caption + Style.space(4)
      Text { anchors.centerIn: parent; visible: !fdEdit.activeFocus; text: fd.readout; color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.caption }
      MouseArea { anchors.fill: parent; visible: !fdEdit.activeFocus; cursorShape: Qt.IBeamCursor; onClicked: fd.openInput() }
      TextInput {
        id: fdEdit
        anchors.fill: parent
        visible: activeFocus
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.caption
        selectByMouse: true
        validator: RegularExpressionValidator { regularExpression: /[-+]?\d*\.?\d*/ }
        onEditingFinished: fd.acceptInput()
      }
    }
    Rectangle {
      id: chn
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      width: Math.max(Style.space(90), parent.width * 0.45)
      height: Style.space(5)
      color: Qt.rgba(0, 0, 0, 0.55)
      border.width: 1
      border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.10)
      Rectangle {
        height: parent.height - 2; y: 1; x: 1
        width: Math.max(0, (parent.width - 2) * fd.frac)
        color: root.dim
      }
      Rectangle {   // the cap
        width: Style.space(8)
        height: Style.space(14)
        y: (parent.height - height) / 2
        x: Math.max(0, Math.min(parent.width - width, (parent.width - width) * fd.frac))
        color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.22)
        border.width: 1
        border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, fdArea.containsMouse || fdArea.pressed ? 0.5 : 0.28)
      }
      MouseArea {
        id: fdArea
        anchors.fill: parent
        anchors.margins: -Style.space(6)
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        property real lastRelease: 0
        function at(mx) {
          var f = Math.max(0, Math.min(1, (mx + Style.space(6)) / chn.width))
          return fd.from + f * (fd.to - fd.from)
        }
        onPressed: function(mouse) { fd.live = at(mouse.x); fd.moved(fd.live) }
        onPositionChanged: function(mouse) { if (pressed) { fd.live = at(mouse.x); fd.moved(fd.live) } }
        onReleased: {
          var now = Date.now()
          if (!isNaN(fd.defaultValue) && now - lastRelease < 350) {
            lastRelease = 0; fd.live = fd.defaultValue; fd.released(fd.defaultValue); return
          }
          lastRelease = now
          fd.released(fd.live)
        }
      }
    }
    onValueChanged: if (!fdArea.pressed) live = value
  }

  // A push button in the console's idiom.
  component Push: Rectangle {
    id: pb
    property string text: ""
    property bool on: false
    property bool hot: false        // mute: the one control that reads as a warning
    property bool enabled: true
    signal clicked()
    implicitWidth: pbT.implicitWidth + Style.space(16)
    implicitHeight: Style.space(22)
    opacity: enabled ? 1 : 0.45
    color: on ? (hot ? Qt.rgba(0.87, 0.58, 0.38, 0.20) : Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.20))
              : Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.05)
    border.width: 1
    border.color: on ? (hot ? "#de9562" : root.accent) : Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.16)
    Eng {
      id: pbT
      anchors.centerIn: parent
      text: pb.text
      tracking: 0.1
      color: pb.on ? (pb.hot ? "#de9562" : root.fg) : root.dim
    }
    MouseArea {
      anchors.fill: parent
      enabled: pb.enabled
      cursorShape: Qt.PointingHandCursor
      onClicked: pb.clicked()
    }
  }

  // ---- the EQ ------------------------------------------------------------
  readonly property var eqBands: (m && m.eq) ? m.eq : []
  property int eqSel: 0
  readonly property var eqTypeLabels: ({ bell: "Bell", lowshelf: "Lo Shelf", highshelf: "Hi Shelf",
                                         highpass: "Hi-Pass", lowpass: "Lo-Pass", notch: "Notch" })
  readonly property var eqGainless: ({ highpass: true, lowpass: true, notch: true })
  readonly property var eqQless: ({ lowshelf: true, highshelf: true })
  function eqSet(i, key, value) {
    if (!svc || i < 0 || i >= eqBands.length) return
    var out = JSON.parse(JSON.stringify(eqBands))
    out[i][key] = value
    svc.setSetting("eq", out)
  }
  function eqAdd() {
    if (!svc || eqBands.length >= 8) return
    var out = JSON.parse(JSON.stringify(eqBands))
    out.push({ on: true, type: "bell", freq: 1000, gain: 0, q: 0.707 })
    eqSel = out.length - 1
    svc.setSetting("eq", out)
  }
  function eqRemove(i) {
    if (!svc || i < 0 || i >= eqBands.length) return
    var out = JSON.parse(JSON.stringify(eqBands))
    out.splice(i, 1)
    if (eqSel >= out.length) eqSel = Math.max(0, out.length - 1)
    svc.setSetting("eq", out)
  }
  // RBJ cookbook, transcribed from Biquad:: in mic.cpp — including the fixed
  // 0.9 shelf slope — so the curve is the filter that is actually running.
  function eqCoeffs(type, rate, f, q, gainDb) {
    var w = 2 * Math.PI * Math.min(f, rate * 0.45) / rate
    var c = Math.cos(w), sn = Math.sin(w), al = sn / (2 * q), a0
    if (type === "highpass") { a0 = 1 + al; return { b0:(1+c)/2/a0, b1:-(1+c)/a0, b2:(1+c)/2/a0, a1:-2*c/a0, a2:(1-al)/a0 } }
    if (type === "lowpass")  { a0 = 1 + al; return { b0:(1-c)/2/a0, b1:(1-c)/a0,  b2:(1-c)/2/a0, a1:-2*c/a0, a2:(1-al)/a0 } }
    if (type === "notch")    { a0 = 1 + al; return { b0:1/a0, b1:-2*c/a0, b2:1/a0, a1:-2*c/a0, a2:(1-al)/a0 } }
    var A = Math.pow(10, gainDb / 40)
    if (type === "lowshelf" || type === "highshelf") {
      var sa = sn / 2 * Math.sqrt((A + 1/A) * (1/0.9 - 1) + 2)
      var sq = 2 * Math.sqrt(A) * sa
      if (type === "lowshelf") {
        a0 = (A+1) + (A-1)*c + sq
        return { b0:A*((A+1)-(A-1)*c+sq)/a0, b1:2*A*((A-1)-(A+1)*c)/a0, b2:A*((A+1)-(A-1)*c-sq)/a0,
                 a1:-2*((A-1)+(A+1)*c)/a0, a2:((A+1)+(A-1)*c-sq)/a0 }
      }
      a0 = (A+1) - (A-1)*c + sq
      return { b0:A*((A+1)+(A-1)*c+sq)/a0, b1:-2*A*((A-1)+(A+1)*c)/a0, b2:A*((A+1)+(A-1)*c-sq)/a0,
               a1:2*((A-1)-(A+1)*c)/a0, a2:((A+1)-(A-1)*c-sq)/a0 }
    }
    a0 = 1 + al/A
    return { b0:(1+al*A)/a0, b1:-2*c/a0, b2:(1-al*A)/a0, a1:-2*c/a0, a2:(1-al/A)/a0 }
  }
  function eqBandDb(co, w) {
    var c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2*w), s2 = Math.sin(2*w)
    var nr = co.b0 + co.b1*c1 + co.b2*c2, ni = -(co.b1*s1 + co.b2*s2)
    var dr = 1 + co.a1*c1 + co.a2*c2,     di = -(co.a1*s1 + co.a2*s2)
    var den = dr*dr + di*di
    return 10 * Math.log10((nr*nr + ni*ni) / (den > 1e-20 ? den : 1e-20))
  }
  function eqCurveDb(f) {
    var rate = 48000, total = 0
    for (var i = 0; i < eqBands.length; i++) {
      var b = eqBands[i]
      if (!b || b.on === false) continue
      total += eqBandDb(eqCoeffs(b.type, rate, b.freq, b.q, b.gain), 2 * Math.PI * Math.min(f, rate * 0.49) / rate)
    }
    return total
  }

  // ---- stage editors -----------------------------------------------------
  // A plain engage switch: the two filter units have nothing else to say.
  Component {
    id: switchUnit
    Column {
      spacing: Style.space(6)
      readonly property string key: root.stage === "hpf" ? "highPass" : "humFilter"
      Push {
        text: "Engage"
        on: root.stage === "hpf" ? !!root.m.highPass : !!root.m.humFilter
        enabled: !!root.svc
        onClicked: root.set(parent.key, !(root.stage === "hpf" ? root.m.highPass : root.m.humFilter))
      }
      Eng {
        text: root.stage === "hpf" ? "80 Hz · rumble and handling"
                                   : "50/60 Hz and first harmonic · mains buzz"
        tracking: 0.06
      }
    }
  }
  Component {
    id: nrUnit
    Column {
      spacing: Style.space(8)
      Push { text: "Engage"; on: !!root.m.voiceIsolation; enabled: !!root.svc
             onClicked: root.set("voiceIsolation", !root.m.voiceIsolation) }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        Knob {
          label: "Amount"; defaultValue: 0.6
          value: root.num("voiceIsolationIntensity", 0.6)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("voiceIsolationIntensity", v) }
        }
      }
      Eng { text: "Adds about 40 ms of delay"; tracking: 0.06 }
    }
  }
  Component {
    id: gateUnit
    Column {
      spacing: Style.space(8)
      Push { text: "Engage"; on: !!root.m.noiseGate; enabled: !!root.svc
             onClicked: root.set("noiseGate", !root.m.noiseGate) }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        Knob {
          label: "Thresh"; defaultValue: 0.55
          value: root.num("noiseGateIntensity", 0.55)
          readout: Math.round(root.gateThresholdDb(live)) + " dB"
          inputMin: -60; inputMax: -15; inputUnit: "dB"
          onReleased: function(v) { root.set("noiseGateIntensity", v) }
        }
      }
    }
  }
  Component {
    id: compUnit
    Column {
      spacing: Style.space(8)
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Push { text: "Main"; on: !!root.m.autoLevel; enabled: !!root.svc
               onClicked: root.set("autoLevel", !root.m.autoLevel) }
        Push { text: "Glue"; on: !!root.m.glueComp; enabled: !!root.svc
               onClicked: root.set("glueComp", !root.m.glueComp) }
      }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Knob {
          label: "Ratio"; defaultValue: 0.6
          value: root.num("autoLevelIntensity", 0.6)
          readout: root.compRatio(live).toFixed(1) + ":1"
          inputMin: 2; inputMax: 6; inputDecimals: 1; inputUnit: ":1"
          onReleased: function(v) { root.set("autoLevelIntensity", v) }
        }
        Knob {
          label: "Thresh"
          value: root.num("autoLevelIntensity", 0.6)
          readout: Math.round(root.compThresholdDb(live)) + " dB"
          inputMin: -14; inputMax: -32; inputUnit: "dB"
          onReleased: function(v) { root.set("autoLevelIntensity", v) }
        }
        Knob {
          label: "Mix"; defaultValue: 1.0
          value: root.num("compMix", 1.0)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("compMix", v) }
        }
      }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        visible: !!root.m.glueComp
        Knob {
          label: "Glue"; defaultValue: 0.4
          value: root.num("glueCompIntensity", 0.4)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("glueCompIntensity", v) }
        }
      }
      Eng { text: "Main catches peaks; Glue holds the vocal in the mix"; tracking: 0.06 }
    }
  }
  Component {
    id: deessUnit
    Column {
      spacing: Style.space(8)
      Push { text: "Engage"; on: !!root.m.deEsser; enabled: !!root.svc
             onClicked: root.set("deEsser", !root.m.deEsser) }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        Knob {
          label: "Amount"; defaultValue: 0.5
          value: root.num("deEsserIntensity", 0.5)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("deEsserIntensity", v) }
        }
      }
    }
  }
  Component {
    id: fxUnit
    Column {
      spacing: Style.space(8)
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Push { text: "Tape"; on: !!root.m.tape; enabled: !!root.svc
               onClicked: root.setFx("tape", !root.m.tape) }
        Push { text: "Ring"; on: !!root.m.ringMod; enabled: !!root.svc
               onClicked: root.setFx("ringMod", !root.m.ringMod) }
        Push { text: "Mega"; on: !!root.m.megaphone; enabled: !!root.svc
               onClicked: root.setFx("megaphone", !root.m.megaphone) }
      }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Knob {
          visible: !!root.m.tape
          label: "Tape"; defaultValue: 0.45
          value: root.num("tapeMix", 0.45)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("tapeMix", v) }
        }
        Knob {
          visible: !!root.m.ringMod
          label: "Ring"; defaultValue: 1.0
          value: root.num("ringModMix", 1.0)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("ringModMix", v) }
        }
        Knob {
          visible: !!root.m.megaphone
          label: "Mega"; defaultValue: 1.0
          value: root.num("megaphoneMix", 1.0)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("megaphoneMix", v) }
        }
      }
      Eng { text: "Independent parallel inserts — stack any combination"; tracking: 0.06 }
    }
  }
  Component {
    id: verbUnit
    Column {
      spacing: Style.space(8)
      Flow {
        width: parent.width
        spacing: Style.space(4)
        Repeater {
          model: root.svc ? root.svc.spaceOptions : []
          delegate: Push {
            required property var modelData
            text: root.spaceLabel(modelData)
            on: (root.m.space || "none") === modelData
            enabled: !!root.svc
            onClicked: root.applyReverbPreset(modelData)
          }
        }
      }
      Column {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(7)
        Row {
          anchors.horizontalCenter: parent.horizontalCenter
          spacing: Style.space(4)
          Knob {
            label: "Size"; defaultValue: 0.5
            value: root.num("spaceSize", 0.5)
            readout: Math.round(live * 100) + "%"
            onReleased: function(v) { root.set("spaceSize", v) }
          }
          Knob {
            label: "Decay"; defaultValue: 0.5
            value: root.num("spaceDecay", 0.5)
            readout: Math.round(live * 100) + "%"
            onReleased: function(v) { root.set("spaceDecay", v) }
          }
          Knob {
            label: "Tone"; defaultValue: 0.5
            value: root.num("spaceTone", 0.5)
            readout: live < 0.34 ? "Dark" : live > 0.66 ? "Bright" : "Warm"
            onReleased: function(v) { root.set("spaceTone", v) }
          }
        }
        Row {
          anchors.horizontalCenter: parent.horizontalCenter
          spacing: Style.space(4)
          Knob {
            label: "Pre"; defaultValue: 0
            value: root.num("spacePreDelay", 0)
            readout: Math.round(live * 120) + " ms"
            inputMin: 0; inputMax: 120; inputUnit: "ms"
            onReleased: function(v) { root.set("spacePreDelay", v) }
          }
          Knob {
            label: "Diffuse"; defaultValue: 0.5
            value: root.num("spaceDiffusion", 0.5)
            readout: Math.round(live * 100) + "%"
            onReleased: function(v) { root.set("spaceDiffusion", v) }
          }
          Knob {
            label: "Lo Cut"; defaultValue: 0
            value: root.num("spaceLowCut", 0)
            readout: live < 0.02 ? "Off" : Math.round(35 + live * 465) + " Hz"
            inputMin: 35; inputMax: 500; inputUnit: "Hz"
            onReleased: function(v) { root.set("spaceLowCut", v) }
          }
        }
        Row {
          anchors.horizontalCenter: parent.horizontalCenter
          spacing: Style.space(4)
          Knob {
            label: "Rate"; defaultValue: 0.5
            value: root.num("spaceModRate", 0.5)
            readout: (0.08 + live * 0.92).toFixed(2) + " Hz"
            inputMin: 0.08; inputMax: 1.0; inputDecimals: 2; inputUnit: "Hz"
            onReleased: function(v) { root.set("spaceModRate", v) }
          }
          Knob {
            label: "Depth"; defaultValue: 0
            value: root.num("spaceModDepth", root.num("spaceMod", 0))
            readout: Math.round(live * 100) + "%"
            onReleased: function(v) { root.set("spaceModDepth", v) }
          }
          Knob {
            label: "Room Mix"; defaultValue: 1.0
            value: root.num("spaceMix", 1.0)
            readout: Math.round(live * 100) + "%"
            onReleased: function(v) { root.set("spaceMix", v) }
          }
        }
        Eng { anchors.horizontalCenter: parent.horizontalCenter; text: "Pick a space, then make it yours"; tracking: 0.06 }
      }
      Rectangle { width: parent.width; height: 1; color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.10) }
      Eng { text: "PARALLEL DELAY SENDS"; tracking: 0.06 }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Push { text: "Slap Send"; on: !!root.m.slapDelay; enabled: !!root.svc
               onClicked: root.set("slapDelay", !root.m.slapDelay) }
        Push { text: "Long Send"; on: !!root.m.longDelay; enabled: !!root.svc
               onClicked: root.set("longDelay", !root.m.longDelay) }
      }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Knob { label: "Slap"; defaultValue: 0.18; value: root.num("slapDelayMix", 0.18)
               readout: Math.round(live * 100) + "%"; onReleased: function(v) { root.set("slapDelayMix", v) } }
        Knob { label: "Slap Time"; defaultValue: 0.4; value: root.num("slapDelayTime", 0.4); inputMin: 70; inputMax: 160; inputUnit: "ms"
               readout: Math.round(70 + live * 90) + " ms"; onReleased: function(v) { root.set("slapDelayTime", v) } }
        Knob { label: "Long"; defaultValue: 0.28; value: root.num("longDelayMix", 0.28)
               readout: Math.round(live * 100) + "%"; onReleased: function(v) { root.set("longDelayMix", v) } }
        Knob { label: "Time"; defaultValue: 0.4; value: root.num("longDelayTime", 0.4); inputMin: 220; inputMax: 1000; inputUnit: "ms"
               readout: Math.round(220 + live * 780) + " ms"; onReleased: function(v) { root.set("longDelayTime", v) } }
      }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Knob { label: "Feedback"; defaultValue: 0.32; value: root.num("longDelayFeedback", 0.32); inputMin: 8; inputMax: 80
               readout: Math.round(8 + live * 72) + "%"; onReleased: function(v) { root.set("longDelayFeedback", v) } }
        Knob { label: "Tail Tone"; defaultValue: 0.45; value: root.num("longDelayTone", 0.45)
               readout: live < 0.34 ? "Dark" : live > 0.66 ? "Bright" : "Warm"; onReleased: function(v) { root.set("longDelayTone", v) } }
      }
      Eng { anchors.horizontalCenter: parent.horizontalCenter; text: "Delay stays separate from the room"; tracking: 0.06 }
    }
  }
  Component {
    id: pitchUnit
    Column {
      spacing: Style.space(8)
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        Knob {
          label: "Pitch"; defaultValue: 0.5
          value: (root.num("pitch", 0) + 12) / 24
          readout: root.semis(live * 24 - 12)
          inputMin: -12; inputMax: 12; inputDecimals: 1; inputUnit: "st"
          onMoved: function(v) { root.set("pitch", Math.round((v * 24 - 12) * 2) / 2) }
          onReleased: function(v) { root.set("pitch", Math.round((v * 24 - 12) * 2) / 2) }
        }
        Knob {
          label: "Formant"; defaultValue: 0.5
          value: (root.num("formant", 0) + 12) / 24
          readout: root.semis(live * 24 - 12)
          inputMin: -12; inputMax: 12; inputDecimals: 1; inputUnit: "st"
          onMoved: function(v) { root.set("formant", Math.round((v * 24 - 12) * 2) / 2) }
          onReleased: function(v) { root.set("formant", Math.round((v * 24 - 12) * 2) / 2) }
        }
        Knob {
          label: "Doubler"; defaultValue: 0.35
          value: root.num("doublerMix", 0.35)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("doublerMix", v) }
        }
      }
      Push { text: "Doubler Send"; on: !!root.m.doubler; enabled: !!root.svc
             onClicked: root.set("doubler", !root.m.doubler) }
      Eng { text: "Doubler is a parallel corrected/delayed vocal aux"; tracking: 0.06 }
      Rectangle { width: parent.width; height: 1; color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.10) }
      Push { text: "Correction"; on: !!root.m.autoTune; enabled: !!root.svc
             onClicked: root.set("autoTune", !root.m.autoTune) }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        visible: !!root.m.autoTune
        Knob {
          label: "Retune"; defaultValue: 0.5
          value: root.num("autoTuneSpeed", 0.5)
          readout: (root.retuneMs(live) >= 10 ? Math.round(root.retuneMs(live)) : root.retuneMs(live).toFixed(1)) + "ms"
          valueToInput: function(v) { return root.retuneMs(v) }
          inputToValue: function(ms) { return Math.log(Math.max(2, Math.min(200, ms)) / 200) / Math.log(0.01) }
          inputDecimals: 1; inputUnit: "ms"
          onReleased: function(v) { root.set("autoTuneSpeed", v) }
        }
        Knob {
          label: "Amount"; defaultValue: 1.0
          value: root.num("autoTuneAmount", 1.0)
          readout: Math.round(live * 100) + "%"
          onReleased: function(v) { root.set("autoTuneAmount", v) }
        }
      }
      Flow {
        width: parent.width
        spacing: Style.space(4)
        visible: !!root.m.autoTune
        Repeater {
          model: root.svc ? root.svc.tuneScaleOptions : []
          delegate: Push {
            required property var modelData
            text: modelData
            on: (root.m.autoTuneScale || "chromatic") === modelData
            enabled: !!root.svc
            onClicked: root.set("autoTuneScale", modelData)
          }
        }
      }
      Eng { visible: !!root.m.autoTune; text: "Set the song key — chromatic does not snap notes"; tracking: 0.05 }
      Flow {
        width: parent.width
        spacing: Style.space(4)
        visible: !!root.m.autoTune && (root.m.autoTuneScale || "chromatic") !== "chromatic"
        Repeater {
          model: root.svc ? root.svc.tuneKeyOptions : []
          delegate: Push {
            required property var modelData
            text: modelData
            on: (root.m.autoTuneKey || "c") === modelData
            enabled: !!root.svc
            onClicked: root.set("autoTuneKey", modelData)
          }
        }
      }
    }
  }

  Component {
    id: eqUnit
    Column {
      spacing: Style.space(7)
      Item {
        width: parent.width
        height: Style.space(22)
        Eng { text: "EQ preset"; anchors.verticalCenter: parent.verticalCenter }
        Push {
          anchors.right: parent.right
          anchors.verticalCenter: parent.verticalCenter
          text: root.activeEqPreset === "custom" ? "Custom ▾" : (root.eqPresets.filter(function(p) { return p.key === root.activeEqPreset })[0] || { label: "Custom" }).label + " ▾"
          on: root.showEqPresets
          enabled: !!root.svc
          onClicked: root.showEqPresets = !root.showEqPresets
        }
      }
      Flow {
        width: parent.width
        spacing: Style.space(4)
        visible: root.showEqPresets
        Repeater {
          model: root.eqPresets
          delegate: Push {
            required property var modelData
            text: modelData.label
            on: root.activeEqPreset === modelData.key
            enabled: !!root.svc
            onClicked: { root.applyEqPreset(modelData.key); root.showEqPresets = false }
          }
        }
      }
      // The response of the filters that are really running, one point per
      // pixel, with a dot per band — selecting a band is a click where you are
      // already looking rather than a trip to a list.
      Canvas {
        id: curve
        width: parent.width
        height: Style.space(78)
        property string key: JSON.stringify(root.eqBands) + "|" + root.eqSel
        onKeyChanged: requestPaint()
        onWidthChanged: requestPaint()
        Component.onCompleted: requestPaint()
        onPaint: {
          var ctx = getContext("2d")
          ctx.reset()
          var w = width, h = height, mid = h / 2, range = 18
          function px(f) { return root.freqToPos(f) * w }
          function py(d) { return mid - Math.max(-range, Math.min(range, d)) / range * (h / 2 - 3) }
          ctx.strokeStyle = Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.07)
          ctx.lineWidth = 1
          var gl = [50, 100, 200, 500, 1000, 2000, 5000, 10000]
          for (var g = 0; g < gl.length; g++) { ctx.beginPath(); ctx.moveTo(px(gl[g]), 0); ctx.lineTo(px(gl[g]), h); ctx.stroke() }
          ctx.strokeStyle = Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.20)
          ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke()
          ctx.strokeStyle = "#e2b969"
          ctx.lineWidth = 2
          ctx.beginPath()
          for (var i = 0; i <= w; i++) {
            var y = py(root.eqCurveDb(root.posToFreq(i / w)))
            if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
          }
          ctx.stroke()
          for (var b = 0; b < root.eqBands.length; b++) {
            var bd = root.eqBands[b]
            if (!bd || bd.on === false) continue
            var bx = px(bd.freq), by = py(root.eqGainless[bd.type] ? 0 : bd.gain)
            ctx.beginPath(); ctx.arc(bx, by, b === root.eqSel ? 5 : 3.5, 0, 2 * Math.PI)
            if (b === root.eqSel) { ctx.fillStyle = root.accent; ctx.fill()
                                    ctx.strokeStyle = root.fg; ctx.lineWidth = 1; ctx.stroke() }
            else { ctx.strokeStyle = Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.55); ctx.lineWidth = 1.5; ctx.stroke() }
          }
        }
        MouseArea {
          anchors.fill: parent
          cursorShape: Qt.PointingHandCursor
          onClicked: function(mouse) {
            // Pick the nearest band on the horizontal axis: the curve is the
            // band list, so clicking near one selects it.
            var best = -1, bestD = 1e9
            for (var b = 0; b < root.eqBands.length; b++) {
              var d = Math.abs(root.freqToPos(root.eqBands[b].freq) * width - mouse.x)
              if (d < bestD) { bestD = d; best = b }
            }
            if (best >= 0 && bestD < Style.space(40)) root.eqSel = best
          }
        }
      }
      Item {
        width: parent.width
        height: Style.space(10)
        Row {
          width: parent.width
          Repeater {
            model: ["20", "200", "1k", "5k", "20k"]
            delegate: Item {
              required property var modelData
              width: parent.width / 5
              height: Style.space(10)
              Eng { text: modelData; tracking: 0.08 }
            }
          }
        }
      }
      Flow {
        width: parent.width
        spacing: Style.space(4)
        Repeater {
          model: root.eqBands.length
          delegate: Push {
            required property int index
            text: String(index + 1)
            on: root.eqSel === index
            enabled: !!root.svc
            onClicked: root.eqSel = index
          }
        }
        Push { text: "+"; enabled: !!root.svc && root.eqBands.length < 8; onClicked: root.eqAdd() }
        Push { text: "−"; enabled: !!root.svc && root.eqBands.length > 0; onClicked: root.eqRemove(root.eqSel) }
      }
      Flow {
        width: parent.width
        spacing: Style.space(4)
        visible: root.eqSel < root.eqBands.length
        Repeater {
          model: root.svc ? root.svc.eqTypeOptions : []
          delegate: Push {
            required property var modelData
            text: root.eqTypeLabels[modelData] || modelData
            on: root.eqSel < root.eqBands.length && (root.eqBands[root.eqSel].type || "bell") === modelData
            enabled: !!root.svc
            onClicked: root.eqSet(root.eqSel, "type", modelData)
          }
        }
      }
      Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Style.space(4)
        visible: root.eqSel < root.eqBands.length
        Knob {
          label: "Freq"; defaultValue: root.freqToPos(1000)
          value: root.eqSel < root.eqBands.length ? root.freqToPos(root.eqBands[root.eqSel].freq) : 0.5
          readout: root.hz(root.posToFreq(live))
          onReleased: function(v) { root.eqSet(root.eqSel, "freq", Math.round(root.posToFreq(v))) }
        }
        Knob {
          label: "Gain"; defaultValue: 0.5
          visible: root.eqSel < root.eqBands.length && !root.eqGainless[root.eqBands[root.eqSel].type]
          value: root.eqSel < root.eqBands.length ? (root.eqBands[root.eqSel].gain + 18) / 36 : 0.5
          readout: root.db(live * 36 - 18)
          onReleased: function(v) { root.eqSet(root.eqSel, "gain", Math.round((v * 36 - 18) * 2) / 2) }
        }
        Knob {
          label: "Q"; defaultValue: (0.707 - 0.1) / 9.9
          visible: root.eqSel < root.eqBands.length && !root.eqQless[root.eqBands[root.eqSel].type]
          value: root.eqSel < root.eqBands.length ? (root.eqBands[root.eqSel].q - 0.1) / 9.9 : 0.06
          readout: (0.1 + live * 9.9).toFixed(2)
          onReleased: function(v) { root.eqSet(root.eqSel, "q", 0.1 + v * 9.9) }
        }
      }
    }
  }

  // ---- bar icon ----------------------------------------------------------
  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    // Muted is the state worth a colour: it is the one that silently costs you
    // the first half of a sentence.
    text: root.muted ? "" : ""
    active: root.muted || root.live
    useActiveColor: true
    activeColor: root.muted ? Color.urgent : root.fg
    tooltipText: (root.muted ? "Microphone muted"
                : root.live ? "Microphone in use"
                            : "Microphone effects") + " · right-click: mute"
    onPressed: function(b) {
      if (b === Qt.RightButton && root.svc) root.svc.toggleMuted()
      else root.toggle()
    }
  }

  onOpenedChanged: {
    if (!svc) return
    svc.setMeter(opened)            // the real mic opens only while we are looking
    if (opened) svc.refresh()
  }
  // The popup can open before the independently loaded service finishes
  // connecting. Re-assert the request when it arrives, otherwise that first
  // opening is permanently meterless until the user closes and reopens it.
  onSvcChanged: if (opened && svc) { svc.setMeter(true); svc.refresh() }

  // ---- the rack ----------------------------------------------------------
  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    // The audio rack has grown into a proper processor; give its controls the
    // breathing room of a desktop plugin rather than the compact camera card.
    contentWidth: panel.fittedContentWidth(Style.space(540))
    contentHeight: panel.fittedContentHeight(rackFrame.implicitHeight)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.userClose()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        // Transforms do not contribute to QML's layout size. This wrapper
        // reports the scaled height so tall units still scroll correctly.
        Item {
          id: rackFrame
          width: parent.width
          implicitHeight: rack.implicitHeight * root.popupScale
          height: implicitHeight
          Column {
            id: rack
            width: parent.width / root.popupScale
            scale: root.popupScale
            transformOrigin: Item.TopLeft

            // -- source ----------------------------------------------------
            RackUnit {
            Item {
              width: parent.width
              height: root.rowH
              Led { id: srcLed; on: root.connected; tint: root.muted ? "#de9562" : root.accent
                    anchors.verticalCenter: parent.verticalCenter }
              Eng { anchors.left: srcLed.right; anchors.leftMargin: Style.space(8)
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.svc ? root.svc.outputLabel : "Microphone Effects"; tracking: 0.24; color: root.fg }
              Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.svc && root.svc.source && root.svc.source.description
                      ? String(root.svc.source.description).slice(0, 22) : "—"
                color: root.dim
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                font.capitalization: Font.AllUppercase
              }
            }
          }

          // -- routing ---------------------------------------------------
          RackUnit {
            Eng { text: "INPUT / VIRTUAL OUTPUT"; tracking: 0.16 }
            Row {
              width: parent.width
              height: root.rowH
              spacing: Style.space(8)
              Eng { text: "Input"; width: Style.space(56); anchors.verticalCenter: parent.verticalCenter }
              ComboBox {
                id: inputPick
                width: parent.width - Style.space(64)
                height: parent.height
                enabled: !!root.svc
                model: root.inputOptions
                textRole: "label"
                currentIndex: root.inputOptionIndex()
                onActivated: function(index) { if (root.svc) root.svc.selectSource(String(root.inputOptions[index].value)) }
                contentItem: Text {
                  leftPadding: Style.space(7)
                  rightPadding: Style.space(20)
                  verticalAlignment: Text.AlignVCenter
                  text: inputPick.displayText
                  color: root.fg
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.body
                  elide: Text.ElideRight
                }
                indicator: Text {
                  x: inputPick.width - width - Style.space(7)
                  anchors.verticalCenter: parent.verticalCenter
                  text: "⌄"
                  color: root.dim
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.body
                }
                background: Rectangle {
                  color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.055)
                  border.width: 1
                  border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, inputPick.activeFocus ? 0.44 : 0.17)
                }
                delegate: ItemDelegate {
                  required property var modelData
                  width: inputPick.width
                  height: root.rowH
                  contentItem: Text {
                    leftPadding: Style.space(7)
                    verticalAlignment: Text.AlignVCenter
                    text: modelData.label
                    color: root.fg
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.body
                    elide: Text.ElideRight
                  }
                  highlighted: inputPick.highlightedIndex === index
                  background: Rectangle { color: parent.highlighted ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.18) : "transparent" }
                }
                popup: Popup {
                  y: inputPick.height
                  width: inputPick.width
                  implicitHeight: contentItem.implicitHeight
                  padding: 1
                  contentItem: ListView {
                    clip: true
                    implicitHeight: Math.min(contentHeight, root.rowH * 6)
                    model: inputPick.popup.visible ? inputPick.delegateModel : null
                    currentIndex: inputPick.highlightedIndex
                    ScrollIndicator.vertical: ScrollIndicator { }
                  }
                  background: Rectangle {
                    color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.10)
                    border.width: 1
                    border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.24)
                  }
                }
              }
            }
            Row {
              width: parent.width
              height: root.rowH
              spacing: Style.space(8)
              Eng { text: "Apps see"; width: Style.space(56); anchors.verticalCenter: parent.verticalCenter }
              Rectangle {
                width: parent.width - Style.space(64)
                height: parent.height
                color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, 0.055)
                border.width: 1
                border.color: Qt.rgba(root.fg.r, root.fg.g, root.fg.b, outputName.activeFocus ? 0.44 : 0.17)
                TextInput {
                  id: outputName
                  anchors.fill: parent
                  anchors.leftMargin: Style.space(7)
                  anchors.rightMargin: Style.space(7)
                  verticalAlignment: TextInput.AlignVCenter
                  color: root.fg
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.body
                  selectByMouse: true
                  text: root.svc ? root.svc.outputLabel : "Microphone Effects"
                  onEditingFinished: {
                    var label = text.trim()
                    if (root.svc && label.length > 0 && label !== root.svc.outputLabel)
                      root.svc.setOutputLabel(label)
                  }
                }
              }
            }
            Eng { text: "Changing the output name reconnects it for a moment"; tracking: 0.04 }
          }

          // -- meters ----------------------------------------------------
          RackUnit {
            Row {
              spacing: Style.space(9)
              Eng { text: "In"; width: Style.space(24); anchors.verticalCenter: parent.verticalCenter }
              SegMeter { level: root.svc ? Math.sqrt(root.svc.inLevel) : 0; anchors.verticalCenter: parent.verticalCenter }
              Eng { text: root.svc ? root.dbfs(root.svc.inLevel) : "—"; anchors.verticalCenter: parent.verticalCenter }
            }
            Row {
              spacing: Style.space(9)
              Eng { text: "Out"; width: Style.space(24); anchors.verticalCenter: parent.verticalCenter }
              SegMeter { level: root.svc ? Math.sqrt(root.svc.outLevel) : 0; anchors.verticalCenter: parent.verticalCenter }
              Eng { text: root.svc ? root.dbfs(root.svc.outLevel) : "—"; anchors.verticalCenter: parent.verticalCenter }
            }
            Item {
              width: parent.width
              height: Style.space(10)
              Row {
                x: Style.space(33)
                width: parent.width - x
                Repeater {
                  model: ["-40", "-20", "-12", "-6", "0", "+3"]
                  delegate: Item {
                    required property var modelData
                    width: (parent.width) / 6
                    height: Style.space(10)
                    Eng { text: modelData; tracking: 0.08; font.pixelSize: Style.font.caption }
                  }
                }
              }
            }
          }

          // -- mute / monitor / level ------------------------------------
          RackUnit {
            Row {
              width: parent.width
              height: Math.max(root.rowH, controls.implicitHeight)
              spacing: Style.space(10)
              Row {
                id: controls
                spacing: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
                Push {
                  text: "Mute"; on: root.muted; hot: true
                  enabled: !!root.svc
                  onClicked: if (root.svc) root.svc.toggleMuted()
                }
                Push {
                  text: "Mon"; on: root.svc ? root.svc.listen : false
                  enabled: !!root.svc && !root.muted
                  onClicked: if (root.svc) root.svc.setListen(!root.svc.listen)
                }
              }
              Fader {
                width: parent.width - controls.implicitWidth - parent.spacing
                height: parent.height
                label: "Level"
                // Use the fader's local travel while it is held, not the
                // daemon's next state packet, so the number follows the cap.
                readout: root.db((live - 0.5) * 36) + " dB"
                value: root.num("volume", 0.5)
                defaultValue: 0.5
                inputMin: -18; inputMax: 18; inputDecimals: 1
                onMoved: function(v) { root.set("volume", v) }
                onReleased: function(v) { root.set("volume", v) }
              }
            }
          }

          // -- channel presets -------------------------------------------
          RackUnit {
            Item {
              width: parent.width
              height: Style.space(12)
              Eng { text: "Channel presets"; anchors.verticalCenter: parent.verticalCenter }
              Eng { text: "restores a complete starting point"; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; tracking: 0.05 }
            }
            Flow {
              width: parent.width
              spacing: Style.space(4)
              Repeater {
                model: root.micPresets
                delegate: Push {
                  required property var modelData
                  text: modelData.label
                  enabled: !!root.svc
                  onClicked: root.applyMicPreset(modelData.key)
                }
              }
            }
          }

          // -- the rail --------------------------------------------------
          RackUnit {
            Item {
              width: parent.width
              height: Style.space(12)
              Eng { text: "Signal chain"; anchors.verticalCenter: parent.verticalCenter }
              Eng {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.engagedCount + " engaged"
                color: root.eng
              }
            }
            Flow {
              width: parent.width
              spacing: Style.space(4)
              Repeater {
                model: root.chain
                delegate: StageModule {
                  required property var modelData
                  stageId: modelData
                  selected: root.stage === modelData
                  engaged: root.stageOn(modelData)
                  onPicked: root.stage = modelData
                }
              }
            }
          }

          // -- the open unit ---------------------------------------------
          RackUnit {
            last: true
            Item {
              width: parent.width
              height: Style.space(20)
              Eng {
                id: unitTitle
                anchors.verticalCenter: parent.verticalCenter
                text: root.stageTitles[root.stage] || root.stage
                tracking: 0.22
                color: root.fg
              }
              Led {
                anchors.left: unitTitle.right
                anchors.leftMargin: Style.space(7)
                anchors.verticalCenter: parent.verticalCenter
                on: root.stageOn(root.stage)
              }
              Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Style.space(4)
                // Reordering: shift this unit one place along the chain. The
                // rail above redraws, so where it landed is visible immediately.
                Push {
                  text: "◀"
                  enabled: !!root.svc && root.chain.indexOf(root.stage) > 0
                  onClicked: if (root.svc) root.svc.moveStage(root.stage, -1)
                }
                Push {
                  text: "▶"
                  enabled: !!root.svc && root.chain.indexOf(root.stage) < root.chain.length - 1
                  onClicked: if (root.svc) root.svc.moveStage(root.stage, 1)
                }
              }
            }
            Loader {
              width: parent.width
              sourceComponent: root.stage === "eq" ? eqUnit
                             : root.stage === "pitch" ? pitchUnit
                             : root.stage === "comp" ? compUnit
                             : root.stage === "gate" ? gateUnit
                             : root.stage === "nr" ? nrUnit
                             : root.stage === "deess" ? deessUnit
                             : root.stage === "fx" ? fxUnit
                             : root.stage === "verb" ? verbUnit
                             : switchUnit
            }
          }
        }
      }
      }
    }
  }
}
