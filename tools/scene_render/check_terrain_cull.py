#!/usr/bin/env python3
"""The ground the GPU culls and draws is the ground in reach.

The ground's draws are made on the GPU (terrain_cull.comp.glsl): which chunks
the view sees, the level each is drawn at, and a skirt on each edge whose
neighbour is drawn at another level. Goldshire's ground, looked over from
above so that the near chunks are drawn at full detail and the far ones at
the coarsest, against the same frame with every chunk in reach drawn and at
full detail (WOWEE_TERRAIN_FRUSTUM_CULL=0, WOWEE_TERRAIN_LOD=0):

- a coarser level strays from the full mesh by under a pixel, so few pixels
  differ by much;
- a chunk the cull left out, or a crack where a skirt is missing, shows the
  sky or the ground behind through the ground - hundreds of pixels.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 640, 360
FRAMES = 12
# A pixel that differs by more than this in a channel is not the same ground
# drawn a little differently.
FAR = 24
# Coarser levels move the ground by under a pixel: along the far ridges a
# few such pixels change by much - 17 here (0.007%). With the skirts left
# out the cracks between levels make 95 (0.04%); a chunk left out, many more.
HOLES = 0.0002


def scene():
    return {
        # Tanaris's dunes outside Gadgetzan: bare ground, little in front of
        # it.
        "map": "Kalimdor", "mapId": 1, "zoneId": 440, "hour": 12.0,
        "server": [-7177.0, -3785.0, 9.0], "terrain": True, "viewDistance": 400,
        # Over the dunes and looking down across them: chunks from under the
        # camera to the edge of reach, at every level.
        "camera": {"offset": [0, 0, 80], "lookAt": [300, 0, 0]},
        "size": [WIDTH, HEIGHT], "frames": FRAMES,
        "rayTracedLighting": 0, "volumetricFog": 0, "shadowQuality": 0, "msaa": 1,
        "bloom": 0, "clouds": False, "sunShafts": False,
        "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, everything=False):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    for key in ("WOWEE_TERRAIN_FRUSTUM_CULL", "WOWEE_TERRAIN_LOD"):
        env.pop(key, None)
    if everything:
        env["WOWEE_TERRAIN_FRUSTUM_CULL"] = "0"
        env["WOWEE_TERRAIN_LOD"] = "0"
    path = scratch / f"{name}.json"
    path.write_text(json.dumps(scene()), encoding="utf-8")
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
    culled = render(binary, scratch, "culled")
    everything = render(binary, scratch, "everything", everything=True)
    pixels = WIDTH * HEIGHT
    far = sum(1 for i in range(pixels)
              if max(abs(culled[i * 3 + k] - everything[i * 3 + k]) for k in range(3)) > FAR)
    share = far / pixels
    print(f"against every chunk in reach at full detail, {far} pixels ({share:.3%}) differ by more than {FAR}")
    if share > HOLES:
        print(f"FAIL: more than {HOLES:.2%} of the frame differs: a chunk the cull dropped, or a crack "
              "where a skirt is missing")
        return 1
    print("PASS: the ground culled and drawn by level on the GPU is the ground in reach")
    return 0


if __name__ == "__main__":
    sys.exit(main())
