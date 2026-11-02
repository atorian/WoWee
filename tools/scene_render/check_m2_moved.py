#!/usr/bin/env python3
"""A doodad moved after it was placed is drawn where it went, and only there,
and so is its shadow.

Where an M2 stands is kept on the GPU, a row per instance, and written again
only when the instance changes; the view and the shadow maps both draw it
from that row. A lamppost on open ground in the late afternoon is placed on
the left and moved to the right, as the client moves a game object, in the
very frame that is kept. Against the same scene with the lamppost hidden
(`"hidden": true`, so the camera and the rest stay the same):

- On the pixels the lamppost and its long shadow cover where it was placed,
  the picture is the empty one: a row left as it was placed would still draw
  it, or cast its shadow, there.
- On the pixels it covers where it went, it is drawn.

Each side's own pixels come from the lamppost placed there and left there.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

LAMPPOST = r"World\Azeroth\Elwynn\PassiveDoodads\LampPost\LampPost.m2"
WIDTH, HEIGHT = 320, 240
PLACED, WENT = [0, -9, 0], [0, 9, 0]
FRAMES = 12
# Moved in the kept frame itself (the last): a pass that draws it from the
# rows as they stood a frame before - recorded ahead of the frame's own
# changes - is caught, not healed by the frames after.
MOVED_AT = FRAMES - 1


def scene(model):
    # The open ground of shadows.json: Elwynn at five, the shadows long.
    return {
        "map": "Azeroth", "mapId": 0, "zoneId": 12, "hour": 17.0,
        "server": [-9440.0, 70.0, 56.5], "streamWorld": False,
        "groundPlane": {"size": 120, "height": 0}, "models": [model],
        "camera": {"offset": [-30, 0, 14], "lookAt": [0, 0, 1]},
        "size": [WIDTH, HEIGHT], "frames": FRAMES, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 2, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False,
        "wind": {"direction": 0, "speed": 0},
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


def differs(a, b, i):
    """Whether pixel `i` differs by more than a rounding step."""
    return max(abs(a[i * 3 + k] - b[i * 3 + k]) for k in range(3)) > 8


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    pixels = range(WIDTH * HEIGHT)
    lamppost = {"path": LAMPPOST, "offset": PLACED}

    empty = render(binary, scratch, "empty", scene(dict(lamppost, hidden=True)))
    placed = render(binary, scratch, "placed", scene(lamppost))
    went = render(binary, scratch, "went", scene(dict(lamppost, offset=WENT)))
    moved = render(binary, scratch, "moved", scene(dict(lamppost, movedAtFrame=MOVED_AT, movedTo=WENT)))

    there = {i for i in pixels if differs(empty, went, i)}
    left = [i for i in pixels if differs(empty, placed, i) and i not in there]
    failures = []
    if len(left) < 100 or len(there) < 100:
        failures.append(f"the lamppost covers {len(left)} pixels where placed and {len(there)} where "
                        "it went; the scene does not show it at both")
    else:
        stayed = sum(1 for i in left if differs(moved, empty, i)) / len(left)
        if stayed > 0.001:
            failures.append(f"{stayed:.1%} of the pixels where it was placed still show it; "
                            "it is drawn where it stood before it moved")
        arrived = sum(1 for i in there if differs(moved, empty, i)) / len(there)
        if arrived < 0.8:
            failures.append(f"only {arrived:.1%} of the pixels where it went show it; "
                            "it is not drawn where it is")
        print(f"placed: {len(left)} pixels, {stayed:.2%} still drawn; went: {len(there)} pixels, "
              f"{arrived:.1%} drawn")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: a moved doodad is drawn where it went")
    return 0


if __name__ == "__main__":
    sys.exit(main())
