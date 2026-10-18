#!/usr/bin/env bash
# Render one model offline and save the frame - no realm, no login.
#
#   tools/model_probe.sh <model.m2|model.wmo> <out.png> [dx,dy,dz,yaw,pitch]
#
# The model goes through the client's own loaders and renderers under
# Dalaran's noon sky. The camera stands relative to the model's centre: yaw 0
# looks along +X, 90 along +Y; without it the camera backs off along -X far
# enough to see the whole model. Shader edits only need their .spv rebuilt
# (ninja -C build-dev wowee_shaders), which also copies them into
# build-dev/bin/assets.
#
# WOWEE_PROBE_CHARACTER=1 draws the model the way a creature or a mount is
# drawn in game, through CharacterRenderer with its emitters in M2Renderer;
# skins a creature display fills in stay white. With it, WOWEE_PROBE_ANIM=<id>
# plays that animation instead of the first sequence (4 walk, 5 run).
#
# Runs build-dev/bin/wowee.exe (WOWEE_BIN overrides) with its working directory
# in that folder, and keeps per-user files under build-dev/scratch so nothing
# lands on the system drive.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${WOWEE_BIN:-$root/build-dev/bin/wowee.exe}"
out="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
scratch="$root/build-dev/scratch/probe"
mkdir -p "$scratch/appdata" "$scratch/config" "$root/build-dev/tmp"

to_native() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

export LOCALAPPDATA="$(to_native "$scratch/appdata")"
# Its own settings, not the player's: those make the window fullscreen, and a
# frame at desktop size costs four times what the default 1280x720 does. Its
# own log too, so a probe beside a running client leaves that client's log be.
export WOWEE_CONFIG_ROOT="$(to_native "$scratch/config")"
export WOWEE_LOG_FILE="${WOWEE_LOG_FILE:-model_probe.log}"
export TMP="$(to_native "$root/build-dev/tmp")" TEMP="$(to_native "$root/build-dev/tmp")"
export WOWEE_SKY_BACKGROUND_PROBE=1
export WOWEE_MODEL_PROBE="$1"
export WOWEE_SCREENSHOT="$(to_native "$out")"
if [[ $# -ge 3 ]]; then export WOWEE_PROBE_CAMERA="$3"; fi

cd "$(dirname "$bin")"
"./$(basename "$bin")" > "$scratch/last_run.log" 2>&1 || true
if [[ -f "$out" ]]; then
    echo "$out"
else
    echo "no frame written; see $scratch/last_run.log and $(dirname "$bin")/logs/$WOWEE_LOG_FILE" >&2
    exit 1
fi
