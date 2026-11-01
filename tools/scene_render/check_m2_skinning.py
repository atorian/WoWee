#!/usr/bin/env python3
"""A skinned doodad is drawn in its own pose, beside others in theirs.

Every skinned M2 drawn in a frame has its bones copied into the frame's bone
buffer at a range of its own, and its vertices are skinned from that range.
Two such doodads stand side by side - a lamppost and the Stormwind
lighthouse's beam, different models with different skeletons - and each is
drawn three ways:

- With the other placed beside it, hidden (`"hidden": true`), so the camera
  and everything else in the scene stay the same: its own picture, and its
  pixels.
- With the other drawn too. On its pixels the picture is its own picture: a
  range that starts in the wrong place hands it the other model's bones, or
  bones of no one.
- With skinning off (WOWEE_M2_NO_SKINNING=1), the bind pose: on its pixels
  the picture is not that, so the bones that reached it are a pose.

Ship machinery (paddle wheels, sails) is left out: it plays in lockstep with
its siblings, so its pose depends on what else is placed.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

LAMPPOST = r"World\Azeroth\Elwynn\PassiveDoodads\LampPost\LampPost.m2"
BEAM = r"World\Generic\Human\Passive Doodads\Stormwind\Stormwind_LighthouseBeam_01.m2"
WIDTH, HEIGHT = 320, 240


def scene(models):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False, "models": models,
        "camera": {"offset": [-14, 4, 4], "lookAt": [0, 4, 3]},
        "size": [WIDTH, HEIGHT], "frames": 12, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False,
        "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source, skinning=True):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    if skinning:
        env.pop("WOWEE_M2_NO_SKINNING", None)
    else:
        env["WOWEE_M2_NO_SKINNING"] = "1"
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
    """Whether pixel `i` changed by more than a rounding step."""
    return max(abs(a[i * 3 + k] - b[i * 3 + k]) for k in range(3)) > 8


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    pixels = range(WIDTH * HEIGHT)
    lamppost = {"path": LAMPPOST}
    beam = {"path": BEAM, "offset": [0, 8, 0]}
    hidden = lambda model: dict(model, hidden=True)

    empty = render(binary, scratch, "empty", scene([hidden(lamppost), hidden(beam)]))
    both = render(binary, scratch, "both", scene([lamppost, beam]))
    bind = render(binary, scratch, "both-bind-pose", scene([lamppost, beam]), skinning=False)
    failures = []
    for name, alone_scene in (("lamppost", [lamppost, hidden(beam)]), ("beam", [hidden(lamppost), beam])):
        alone = render(binary, scratch, f"{name}-alone", scene(alone_scene))
        own = [i for i in pixels if moved(empty, alone, i)]
        if len(own) < 100:
            failures.append(f"{name}: {len(own)} pixels drawn alone; it is not drawn")
            continue
        changed = sum(1 for i in own if moved(both, alone, i)) / len(own)
        if changed > 0.001:
            failures.append(f"{name}: {changed:.1%} of its pixels differ beside the other; "
                            "it was drawn with bones not its own")
        posed = sum(1 for i in own if moved(both, bind, i)) / len(own)
        if posed < 0.2:
            failures.append(f"{name}: only {posed:.1%} of its pixels move off the bind pose; "
                            "its bones did not reach it")
        print(f"{name}: {len(own)} pixels, {changed:.2%} changed beside the other, {posed:.1%} off the bind pose")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: each skinned doodad drawn in its own pose")
    return 0


if __name__ == "__main__":
    sys.exit(main())
