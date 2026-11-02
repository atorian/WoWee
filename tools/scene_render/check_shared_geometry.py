#!/usr/bin/env python3
"""Models whose geometry shares a page are each drawn as themselves.

A renderer keeps its models' vertices and indices in shared pages
(GeometryPool): the first model loaded starts at the page's first vertex and
index, every later one further in, and each draw names its model's base
vertex and first index. Two different models stand side by side and are drawn
three ways:

- each alone in the scene, so it is the only model loaded and starts at the
  page's beginning: its own picture, and its pixels;
- both, the first listed loaded first, so the second starts further in. On
  each one's pixels the picture is its own picture: a draw that misses its
  model's base vertex or first index draws the other model's triangles, or
  none.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 320, 240
# What each renderer is given, and where the camera stands to see both.
OBJECTS = {
    "m2": {"key": "models",
           "first": {"path": r"World\Azeroth\Elwynn\PassiveDoodads\LampPost\LampPost.m2"},
           "second": {"path": r"World\Generic\Human\Passive Doodads\Banners\StormwindGriffonBanner01.m2",
                      "offset": [0, 6, 0]},
           "camera": {"offset": [-12, 3, 3], "lookAt": [0, 3, 2]}},
    "character": {"key": "characters",
                  "first": {"path": r"Creature\Wolf\Wolf.m2", "offset": [0, -2, 0]},
                  "second": {"path": r"Creature\Bear\Bear.m2", "offset": [0, 2, 0]},
                  "camera": {"offset": [-9, 0, 3], "lookAt": [0, 0, 1]}},
}


def scene(kind, placed):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": [], OBJECTS[kind]["key"]: placed, "camera": OBJECTS[kind]["camera"],
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


def moved(a, b, i):
    """Whether pixel `i` is something else. The frame's exposure follows
    what is in it, so a model beside another is a few steps darker all over
    (up to 11 here); other triangles in its place are tens of steps off."""
    return max(abs(a[i * 3 + k] - b[i * 3 + k]) for k in range(3)) > 24


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--object", choices=sorted(OBJECTS), required=True)
    args = parser.parse_args()
    binary, scratch = args.binary.resolve(), args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    kind = args.object
    first, second = OBJECTS[kind]["first"], OBJECTS[kind]["second"]
    pixels = range(WIDTH * HEIGHT)

    empty = render(binary, scratch, "empty", scene(kind, []))
    both = render(binary, scratch, "both", scene(kind, [first, second]))
    failures = []
    for name, model in (("first", first), ("second", second)):
        alone = render(binary, scratch, f"{name}-alone", scene(kind, [model]))
        own = [i for i in pixels if moved(empty, alone, i)]
        if len(own) < 100:
            failures.append(f"{model['path']}: {len(own)} pixels drawn alone; it is not drawn")
            continue
        changed = sum(1 for i in own if moved(both, alone, i)) / len(own)
        if changed > 0.001:
            failures.append(f"{model['path']}: {changed:.1%} of its pixels differ beside the other; "
                            "it was drawn from geometry not its own")
        print(f"{model['path']}: {len(own)} pixels, {changed:.2%} changed beside the other")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print(f"PASS: each {kind} drawn as itself from the shared geometry pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
