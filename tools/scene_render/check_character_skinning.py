#!/usr/bin/env python3
"""A creature is drawn in the pose its animation puts it in.

A unit's skeleton is posed on the GPU (skeleton.comp.glsl): every bone's
tracks sampled at the unit's time, composed onto its parent's matrix, and
the mesh skinned by them. A wolf standing on its own, seen from the side
after twelve frames of its stand, is drawn; its silhouette is where the
frame differs from the same frame without it.

reference/wolf_stand_silhouette.png is that silhouette as the client drew
it while its skeletons were still posed on the CPU (master 50a0693d,
CharacterRenderer::calculateBoneMatrices), white where the wolf is. The GPU
pose must lay the wolf over it: a bone composed on no parent, a key read
from the wrong place or a bone left at its bind pose moves legs, tail or
head off it - 0.59 of it shared with the parents dropped, 0.52 unposed.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 320, 240
WOLF = r"Creature\Wolf\Wolf.m2"
REFERENCE = Path(__file__).resolve().parent / "reference" / "wolf_stand_silhouette.png"
# A pixel this far off in a channel is the wolf's.
APART = 24
# The share of the two silhouettes' union they have in common, at least:
# the same pose drawn on another GPU differs at the edges only.
SHARED = 0.95


def scene(characters):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False, "models": [], "characters": characters,
        "camera": {"offset": [0, -6, 1.2], "lookAt": [0, 0, 0.8]},
        "size": [WIDTH, HEIGHT], "frames": 12, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False, "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    path = scratch / f"{name}.json"
    path.write_text(json.dumps(source), encoding="utf-8")
    png = scratch / f"{name}.png"
    run = subprocess.run([str(binary), str(path), str(png)], cwd=binary.parent, env=env, capture_output=True)
    output = run.stdout + run.stderr
    (scratch / f"{name}.log").write_bytes(output)
    if b"no Vulkan device" in output or b"no assets under" in output:
        raise SystemExit(77)
    if run.returncode:
        print(output.decode(errors="replace")[-4000:])
        raise SystemExit(1)
    if b"VUID-" in output or b"Validation Error:" in output:
        print("FAIL: Vulkan validation error; see", scratch / f"{name}.log")
        raise SystemExit(1)
    return rgb_pixels(png)


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    empty = render(binary, scratch, "empty", scene([]))
    posed = render(binary, scratch, "posed", scene([{"path": WOLF}]))
    reference = rgb_pixels(REFERENCE)
    pixels = range(WIDTH * HEIGHT)
    drawn = {i for i in pixels if max(abs(posed[i * 3 + k] - empty[i * 3 + k]) for k in range(3)) > APART}
    expected = {i for i in pixels if reference[i * 3] > 127}
    if len(drawn) < 200:
        print(f"FAIL: the wolf is not drawn ({len(drawn)} pixels)")
        return 1
    shared = len(drawn & expected) / len(drawn | expected)
    print(f"wolf {len(drawn)} pixels, the CPU-posed one {len(expected)}; {shared:.3f} of both shared")
    if shared < SHARED:
        print(f"FAIL: under {SHARED} shared - the wolf is not in its stand's pose")
        return 1
    print("PASS: the wolf stands in the pose its animation puts it in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
