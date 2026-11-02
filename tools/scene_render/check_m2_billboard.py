#!/usr/bin/env python3
"""A billboard card faces the camera whichever way its model faces.

Mana Shield's card hangs from a root bone that is a billboard locked to Z
(flags 0x240): the GPU poses it (skeleton.comp.glsl) turned about its own Z
axis to the view, after the model's placement. The model stands before a
fixed camera turned 0, 90 and 180 degrees; its card's silhouette - where the
frame differs from the same frame with the model hidden - is the same each
time.

Left as authored (WOWEE_M2_NO_BILLBOARD=1), the card faces the model's +X,
east, and is seen edge-on from the south: the run says so, or the scene
could not tell a turned card from one that is not.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 240, 240
SHIELD = r"Spells\ManaShield_State_Base.m2"
# A pixel this far off in a channel is the card's.
APART = 16
# The share of two silhouettes' union they have in common: at least this
# when the card faces the camera both times, at most UNTURNED when one of
# them is edge-on.
SAME = 0.9
UNTURNED = 0.6


def scene(yaw, hidden=False):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": [{"path": SHIELD, "offset": [0, 0, 1], "rotation": [0, 0, yaw], "hidden": hidden}],
        # South of it, a little above, looking at it.
        "camera": {"offset": [0, -8, 3], "lookAt": [0, 0, 1]},
        "size": [WIDTH, HEIGHT], "frames": 12, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False, "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source, billboards=True):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    # The card alone: its particles are drawn at random.
    env["WOWEE_M2_NO_PARTICLES"] = "1"
    if billboards:
        env.pop("WOWEE_M2_NO_BILLBOARD", None)
    else:
        env["WOWEE_M2_NO_BILLBOARD"] = "1"
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


def silhouette(frame, empty):
    return {i for i in range(WIDTH * HEIGHT)
            if max(abs(frame[i * 3 + k] - empty[i * 3 + k]) for k in range(3)) > APART}


def shared(a, b):
    union = a | b
    return len(a & b) / len(union) if union else 1.0


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    empty = render(binary, scratch, "empty", scene(0, hidden=True))
    facing = {yaw: silhouette(render(binary, scratch, f"yaw{yaw}", scene(yaw)), empty) for yaw in (0, 90, 180)}
    failures = []
    if len(facing[0]) < 200:
        failures.append(f"the card covers {len(facing[0])} pixels; it is not drawn")
    for yaw in (90, 180):
        common = shared(facing[0], facing[yaw])
        print(f"turned {yaw}: {len(facing[yaw])} pixels, {common:.3f} shared with unturned")
        if common < SAME:
            failures.append(f"turned {yaw} degrees the card shares {common:.3f} of its silhouette (< {SAME}); "
                            "it did not turn to the camera")
    authored = silhouette(render(binary, scratch, "yaw0-authored", scene(0), billboards=False), empty)
    common = shared(facing[0], authored)
    print(f"left as authored: {len(authored)} pixels, {common:.3f} shared")
    if common > UNTURNED:
        failures.append(f"left as authored, the card still shares {common:.3f} (> {UNTURNED}); "
                        "the scene cannot see the card turn")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: the card faces the camera whichever way its model faces")
    return 0


if __name__ == "__main__":
    sys.exit(main())
