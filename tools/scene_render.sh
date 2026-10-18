#!/usr/bin/env bash
# Render a static scene to a PNG - no realm, no login, no client.
#
#   tools/scene_render.sh <scene.json> <out.png>
#
# The scene file is described at the top of tools/scene_render/main.cpp; there
# are examples in tools/scene_render/scenes. Build it before a run, after a
# renderer or a shader edit alike - it compiles the changed shaders and copies
# them and the rest of assets/ into build-dev/bin, where it loads them from:
#
#   ninja -C build-dev scene_render
#
# Runs build-dev/bin/scene_render.exe (SCENE_RENDER_BIN overrides) with its
# working directory in that folder, where Data and the shaders are, and keeps
# temporaries under build-dev so nothing lands on the system drive.
#
# SCENE_RENDER_TIMING=1 prints each frame's CPU and GPU time, and the frame
# budget of the frames after the warm-up, before the PNG's line.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
timing=()
if [[ "${SCENE_RENDER_TIMING:-0}" != "0" ]]; then timing=(--timing); fi

root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${SCENE_RENDER_BIN:-$root/build-dev/bin/scene_render.exe}"
scene="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
out="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
mkdir -p "$root/build-dev/tmp" "$root/build-dev/scratch"

to_native() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

export TMP="$(to_native "$root/build-dev/tmp")" TEMP="$(to_native "$root/build-dev/tmp")"

cd "$(dirname "$bin")"
if ! "./$(basename "$bin")" "$(to_native "$scene")" "$(to_native "$out")" "${timing[@]}" \
        > "$root/build-dev/scratch/scene_render.log" 2>&1; then
    tail -20 "$root/build-dev/scratch/scene_render.log" >&2
    exit 1
fi
if [[ ${#timing[@]} -gt 0 ]]; then
    grep '^timing: ' "$root/build-dev/scratch/scene_render.log" | sed 's/^timing: //' || true
fi
tail -1 "$root/build-dev/scratch/scene_render.log"
