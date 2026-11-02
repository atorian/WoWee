#!/usr/bin/env python3
"""A tree's shadow is cut by its leaves.

A canopy's leaves are alpha-keyed cards: the batch samples its texture and
drops what the key cuts, in the main pass and in the shadow map alike. On
the ground the canopy's shadow is then dappled - light between the leaves -
and the dappling is where the shadow has edge. An Elwynn canopy stands on a
flat plane in the late afternoon; the camera looks down on its long shadow,
with the tree itself out of view. The shadow is where the plane is darker
than with the tree hidden (placed and not drawn, so the light is the same).

- Its leaves cut: a ragged shadow, 0.049 of its pixels on its edge.
- Every card cast whole (the alpha test lost on the way to the shadow
  pass): a few large shapes, 0.014.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 320, 320
TREE = r"World\Azeroth\Elwynn\PassiveDoodads\Trees\ElwynnTreeCanopy01.m2"
# A pixel this much darker in all three channels together is in shadow.
SHADOW = 60
# Left of this column only: the canopy's own leaves reach into the corner
# beyond it.
COLUMNS = 220
# The share of the shadow on its edge below which it is leaf cards cast whole.
DAPPLED = 0.03


def scene(hidden):
    return {
        "map": "Azeroth", "mapId": 0, "zoneId": 12, "hour": 17.0, "server": [-9440.0, 70.0, 56.5],
        "groundPlane": {"size": 200, "height": 0},
        "models": [{"path": TREE, "hidden": hidden}],
        # Above the shadow it throws toward -x, -y, looking down.
        "camera": {"offset": [-24, -25, 40], "lookAt": [-24, -24, 0]},
        "size": [WIDTH, HEIGHT], "frames": 30, "shadowQuality": 2, "rayTracedLighting": 0,
        "clouds": False, "bloom": 0, "sunShafts": False, "volumetricFog": 0,
        "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
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
    tree = render(binary, scratch, "tree", scene(False))
    bare = render(binary, scratch, "bare", scene(True))

    def shaded(x, y):
        i = (y * WIDTH + x) * 3
        return sum(bare[i:i + 3]) - sum(tree[i:i + 3]) > SHADOW

    mask = [[shaded(x, y) for x in range(COLUMNS)] for y in range(HEIGHT)]
    area = sum(sum(row) for row in mask)
    edge = sum(1 for y in range(1, HEIGHT - 1) for x in range(1, COLUMNS - 1)
               if mask[y][x] and not (mask[y - 1][x] and mask[y + 1][x] and mask[y][x - 1] and mask[y][x + 1]))
    if area < 1000:
        print(f"FAIL: {area} pixels in shadow; the tree cast none")
        return 1
    share = edge / area
    print(f"shadow {area} pixels, {edge} on its edge ({share:.3f})")
    if share < DAPPLED:
        print(f"FAIL: under {DAPPLED} of the shadow is edge - its leaf cards cast whole")
        return 1
    print("PASS: the canopy's shadow is cut by its leaves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
