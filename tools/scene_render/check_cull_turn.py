#!/usr/bin/env python3
"""What comes into view when the camera turns is drawn in the frame it turns.

The GPU culls the scene's tables and its verdict reaches the CPU a frame or
two later, so what a pass draws must not depend on what an earlier frame
saw. Goldshire's ground and its doodads, streamed as the client streams
them, turned two ways for the kept frame:

- a flick, from looking west to looking east at once: no verdict of the
  frames before covers that view (cull_guard.hpp), and the frame is culled
  on the CPU;
- a nudge of ten degrees, inside the guard the GPU culled with: the frame is
  culled from the GPU's verdict, which must already hold what came into view.

Against the same scene looking there all along (as many frames, the clock
the same), culled on the CPU alone (WOWEE_CULL_ON_CPU=1) so that no verdict
shapes it, each kept frame differs only by a little: a tree that was not
drawn, or sky where the ground should be, differs by a lot.
"""
import math
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 320, 240
FRAMES = 12
EAST, WEST = [60, 0, 2], [-60, 0, 2]
NUDGE = math.radians(10)
NUDGED = [60 * math.cos(NUDGE), 60 * math.sin(NUDGE), 2]
# A pixel that differs by more than this in a channel is not the same thing
# lit a little differently. Turned and looking there all along draw the
# same frame - none differ - so a few such pixels are already a hole: one
# bush left out is several hundred.
FAR = 48
HOLES = 0.001


def scene(look_at, last=None, frames=FRAMES):
    camera = {"offset": [0, 0, 6], "lookAt": look_at}
    if last:
        camera["lookAtLast"] = last
    return {
        "map": "Azeroth", "mapId": 0, "zoneId": 12, "hour": 12.0,
        # The client derives the tiles it streams from the view distance once
        # the map's distant terrain is in; the shortest keeps them few, and
        # the frames wait as long as they take.
        "server": [-9440.0, 70.0, 56.5], "terrain": True, "viewDistance": 400, "settleFrames": 6000,
        "camera": camera, "size": [WIDTH, HEIGHT], "frames": frames,
        "rayTracedLighting": 0, "volumetricFog": 0, "shadowQuality": 2, "msaa": 1,
        "bloom": 0, "clouds": False, "sunShafts": False,
        "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source, on_cpu=False):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env.pop("WOWEE_CULL_ON_CPU", None)
    if on_cpu:
        env["WOWEE_CULL_ON_CPU"] = "1"
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
    return rgb_pixels(png), output.decode(errors="replace")


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)

    failures = []
    # (name, aimed before, aimed in the kept frame, how the frame must be culled)
    for name, before, after, culled in (("flick", WEST, EAST, "columns"),
                                         ("nudge", EAST, NUDGED, "GPU verdict")):
        # The turn's kept frame is frame FRAMES; the one looking there all
        # along keeps the same frame.
        turned, log = render(binary, scratch, f"{name}-turned", scene(before, last=after))
        along, _ = render(binary, scratch, f"{name}-along", scene(after, frames=FRAMES + 1), on_cpu=True)

        pixels = WIDTH * HEIGHT
        far = sum(1 for i in range(pixels)
                  if max(abs(turned[i * 3 + k] - along[i * 3 + k]) for k in range(3)) > FAR)
        share = far / pixels
        print(f"{name}: against looking there all along, {far} pixels ({share:.2%}) differ by more than {FAR}")
        if share > HOLES:
            failures.append(f"{name}: more than {HOLES:.0%} of the frame is not what looking there shows; "
                            "what came into view with the turn is missing")
        if f"world M2 view culled from the {culled} in the kept frame" not in log:
            failures.append(f"{name}: the kept frame's M2s were not culled from the {culled}, "
                            "so this case does not test what it is for")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: what comes into view when the camera turns is drawn at once")
    return 0


if __name__ == "__main__":
    sys.exit(main())
