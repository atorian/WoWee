#!/usr/bin/env python3
"""A doodad moved after it was placed is drawn where it went, and only there.

Where an M2 stands is kept on the GPU, a row per instance, and written again
only when the instance changes. A lamppost is placed on the left and, some
frames in, moved to the right, as the client moves a game object; the kept
frame is drawn well after that. Against the same scene with the lamppost
hidden (`"hidden": true`, so the camera and the rest stay the same):

- On the pixels the lamppost covers where it was placed, the picture is the
  empty one: a row left as it was placed would still draw it there.
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
PLACED, WENT = [0, -6, 0], [0, 6, 0]
MOVED_AT = 6


def scene(model):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False, "models": [model],
        "camera": {"offset": [-20, 0, 4], "lookAt": [0, 0, 3]},
        "size": [WIDTH, HEIGHT], "frames": 12, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
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
