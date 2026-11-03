#!/usr/bin/env python3
"""A character is drawn batch by batch as its animation and its fade say.

The cull on the GPU (char_draws.comp.glsl) decides per frame what the CPU
decided per batch before: a batch whose animated alpha is nothing is not
drawn, and an instance faded below opaque goes to the faded part, its solid
batches into depth first and then in colour only where that depth is.

The woodcutter (Creature\\HumanMalePeasant\\HumanMalePeasantWood.m2) carries
one of his two bundles of logs in his stand; the other is a batch of seven
whose colour track holds it at nothing there. Drawn opaque he is six draws
in the solid part. Faded to a half he is six batches twice in the faded
part - depth, then colour - and every pixel of him is half his opaque colour
over half the empty scene's: one layer of him, not his far side through his
near one.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 320, 240
WOODCUTTER = r"Creature\HumanMalePeasant\HumanMalePeasantWood.m2"
# A pixel this far off in a channel is the woodcutter's.
APART = 24
# A faded pixel's channels this close to halfway between his opaque colour
# and the empty scene's, on average and nearly everywhere.
MEAN_OFF = 3.0
OFF = 8
NEARLY_EVERYWHERE = 0.97


def scene(characters):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False, "models": [], "characters": characters,
        "camera": {"offset": [0, -5, 1.2], "lookAt": [0, 0, 0.9]},
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
    counts = re.search(rb"character draws in the kept frame: (\d+) solid, (\d+) faded", output)
    if not counts:
        print("FAIL: scene_render said nothing of the character draws; see", scratch / f"{name}.log")
        raise SystemExit(1)
    return rgb_pixels(png), (int(counts.group(1)), int(counts.group(2)))


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    empty, _ = render(binary, scratch, "empty", scene([]))
    opaque, opaque_counts = render(binary, scratch, "opaque", scene([{"path": WOODCUTTER}]))
    faded, faded_counts = render(binary, scratch, "faded", scene([{"path": WOODCUTTER, "opacity": 0.5}]))
    failed = False
    print(f"opaque: {opaque_counts[0]} solid, {opaque_counts[1]} faded draws")
    if opaque_counts != (6, 0):
        print("FAIL: opaque, the woodcutter is six draws in the solid part - his hidden bundle not among them")
        failed = True
    print(f"faded: {faded_counts[0]} solid, {faded_counts[1]} faded draws")
    if faded_counts != (0, 12):
        print("FAIL: faded, the woodcutter is twelve draws in the faded part - depth, then colour")
        failed = True

    pixels = range(WIDTH * HEIGHT)
    body = [i for i in pixels if max(abs(opaque[i * 3 + k] - empty[i * 3 + k]) for k in range(3)) > APART]
    if len(body) < 200:
        print(f"FAIL: the woodcutter is not drawn ({len(body)} pixels)")
        return 1
    offs = [max(abs(2 * faded[i * 3 + k] - opaque[i * 3 + k] - empty[i * 3 + k]) / 2 for k in range(3))
            for i in body]
    mean = sum(offs) / len(offs)
    near = sum(1 for off in offs if off <= OFF) / len(offs)
    print(f"faded over {len(body)} pixels: {mean:.2f} from halfway on average, {near:.3f} within {OFF}")
    if mean > MEAN_OFF or near < NEARLY_EVERYWHERE:
        print("FAIL: faded, the woodcutter is not one layer of him at half over the scene")
        failed = True
    if failed:
        return 1
    print("PASS: the woodcutter is drawn as his animation and his fade say")
    return 0


if __name__ == "__main__":
    sys.exit(main())
