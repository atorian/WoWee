#!/usr/bin/env bash
# Render one model offline and save the frame - no realm, no login.
#
#   tools/model_probe.sh <model.m2|model.wmo> <out.png> [dx,dy,dz,yaw,pitch]
#
# The model goes through the client's own loaders and renderers under
# Dalaran's noon sky. The camera stands relative to the model's centre: yaw 0
# looks along +X, 90 along +Y; without it the camera backs off along -X far
# enough to see the whole model. Shader edits only need their .spv rebuilt
# (ninja -C build-dev wowee_shaders) and copied into build-dev/bin/assets.
#
# Runs build-dev/bin/wowee.exe (WOWEE_BIN overrides) with its working directory
# in that folder, and keeps per-user files under build-dev/scratch so nothing
# lands on the system drive.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${WOWEE_BIN:-$root/build-dev/bin/wowee.exe}"
out="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
scratch="$root/build-dev/scratch/probe"
mkdir -p "$scratch/appdata" "$root/build-dev/tmp"

to_native() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

export LOCALAPPDATA="$(to_native "$scratch/appdata")"
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
    echo "no frame written; see $scratch/last_run.log and $(dirname "$bin")/logs/wowee.log" >&2
    exit 1
fi
