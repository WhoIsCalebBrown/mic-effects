# Microphone Effects for Omarchy

A system-wide virtual microphone and a console-style effects rack for the
Omarchy Shell. Pick **Microphone Effects** in Zoom, Meet, Discord, OBS, or any
other PipeWire application, then shape the sound from the bar.

Everything runs locally on the CPU. The plugin does not access the network,
and unlike a virtual-camera plugin it needs no root setup.

## Features

- Input/output meters, mute, monitor, input selection, and output naming
- Reorderable high-pass, hum removal, noise reduction, gate, compressor,
  de-esser, parametric EQ, pitch, voice-FX, and reverb stages
- Auto-tune with key, scale, speed, and amount controls
- Pitch and formant shifting, doubler, tape, ring modulation, and megaphone
- Room, hall, cathedral, echo, and underwater spaces
- Slap and long delays, parallel compression, dry/wet controls, and presets
- Optional per-microphone settings

## Install

```bash
omarchy plugin add https://github.com/WhoIsCalebBrown/mic-effects.git --enable
```

The service builds its small C++ PipeWire daemon on first load. Omarchy already
ships the usual build tools; if any are missing, the log names the exact
packages and the command to install them:

```bash
cat ~/.cache/mic-effects/install.log
```

The runtime depends on `pipewire`; building requires `gcc`, `make`, and
`pkgconf`. No OpenCV, ONNX models, kernel module, or privileged helper is used.

When installed on a machine that previously used the microphone built into
Camera Effects, the installer imports that microphone configuration once and
leaves the original file untouched.

## Use

Click the microphone icon to open the rack. Right-click the icon to mute. A lit
stage in the signal-chain rail is engaged; select one to edit it, or use the
arrow buttons to move it earlier or later in the chain. Double-click a knob or
fader to return it to its default.

For feedback-free monitoring, use headphones before enabling **Mon**.

The daemon is also available from a terminal:

```bash
mic-effects-server status
mic-effects-server quit
```

## Camera Effects compatibility

This project began as the microphone half of
[alanfortlink/camera-effects](https://github.com/alanfortlink/camera-effects).
It is now independent: it owns its daemon, socket, settings, PipeWire node, and
build. If Camera Effects is also installed, use only one of the two virtual
microphones. A companion Camera Effects change that disables its legacy mic
provider when this plugin is present is planned for upstream submission.

If Camera Effects previously hid your raw microphones, turn that option off
before removing it. This plugin's own optional hide helper uses separately
named WirePlumber files and never enables itself automatically.

## Development

```bash
omarchy plugin validate .
make -C daemon clean all
./install.sh
```

The QML plugin hot-reloads when developed from
`~/.config/omarchy/plugins/whoiscalebbrown.mic-effects/`. The daemon and its
state are deliberately separate:

- socket: `$XDG_RUNTIME_DIR/mic-effects/ctl.sock`
- settings: `$XDG_CONFIG_HOME/mic-effects/config.json`
- installed runtime: `~/.local/lib/mic-effects/`

## Credits and license

The original PipeWire integration and microphone DSP were created in Camera
Effects by tank (alanfortlink). Caleb Brown extracted the audio system and
developed the console rack, reorderable processing, parametric EQ, pitch and
formant controls, advanced vocal effects, presets, and standalone lifecycle.

MIT licensed. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
