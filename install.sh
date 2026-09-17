#!/bin/bash
# Build and install the unprivileged Microphone Effects runtime for this user.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
LIB=${XDG_LIB_HOME:-$HOME/.local/lib}/mic-effects
DATA=${XDG_DATA_HOME:-$HOME/.local/share}/mic-effects
BIN=$HOME/.local/bin
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/mic-effects
MODE=${1:-}

if [[ $MODE == --uninstall ]]; then
  if [[ -x $LIB/mic-effects-hide ]]; then
    "$LIB/mic-effects-hide" none >/dev/null 2>&1 || true
  fi
  rm -f "$BIN/mic-effects-server"
  rm -rf "$LIB" "$DATA" "$CACHE"
  echo "Removed the runtime. Plugin settings remain in ~/.config/mic-effects/."
  exit 0
fi

missing=()
for package in gcc make pkgconf pipewire; do
  pacman -Q "$package" >/dev/null 2>&1 || missing+=("$package")
done
if ((${#missing[@]})); then
  echo "Missing build dependencies: ${missing[*]}" >&2
  echo "Install them with: omarchy pkg add ${missing[*]}" >&2
  exit 1
fi

BUILD=$CACHE/build
LOG=$CACHE/build.log
mkdir -p "$BUILD"
if ! make -C "$HERE/daemon" BUILD="$BUILD" -j"$(nproc)" >"$LOG" 2>&1; then
  tail -n 20 "$LOG" >&2
  echo "Build failed; full log: $LOG" >&2
  exit 1
fi

install -d "$LIB" "$DATA/wireplumber" "$BIN"
install -m 755 "$BUILD/mic-effects-server" "$LIB/mic-effects-server"
install -m 755 "$HERE/scripts/mic-effects-hide" "$LIB/mic-effects-hide"
install -m 644 "$HERE/wireplumber/mic-effects-hide-mics.lua" "$DATA/wireplumber/mic-effects-hide-mics.lua"
ln -sfn "$LIB/mic-effects-server" "$BIN/mic-effects-server"
git -C "$HERE" rev-parse HEAD >"$LIB/installed-commit" 2>/dev/null || date +%s >"$LIB/installed-commit"

# Adopt the microphone settings from Camera Effects once, when this plugin has
# no configuration of its own. The original file is left untouched.
NEW_CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/mic-effects/config.json
OLD_CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/camera-effects/config.json
if [[ ! -e $NEW_CONFIG && -f $OLD_CONFIG ]] && command -v jq >/dev/null; then
  mkdir -p "$(dirname "$NEW_CONFIG")"
  if jq -e '.mic | type == "object"' "$OLD_CONFIG" >/dev/null 2>&1; then
    jq '.mic' "$OLD_CONFIG" >"$NEW_CONFIG.tmp"
    chmod 600 "$NEW_CONFIG.tmp"
    mv "$NEW_CONFIG.tmp" "$NEW_CONFIG"
    echo "Imported existing microphone settings from Camera Effects."
  fi
fi

echo "Microphone Effects runtime installed."
