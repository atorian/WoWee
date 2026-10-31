#!/usr/bin/env python3
"""Which faces of a model are drawn: the material decides, as in the client.

A material without the two-sided flag (M2 0x04, WMO F_UNCULLED) shows its front
faces only, the side its triangles wind counter-clockwise towards. Each check
renders a view twice, as drawn and with every face kept (WOWEE_FACE_CULLING=0):

- Inside a closed one-sided doodad every face is a back face, and the picture
  is the empty scene's; inside a two-sided one its walls are seen.
- A pillar's faces look outward: from outside, and in the water's reflection,
  culling changes nothing - the near faces drawn are the front ones, and a
  winding taken the wrong way round would draw the far ones. From inside its
  walls are gone.
- A room's faces look inward (cameron.wmo, an interior test room): from inside
  culling changes nothing; from outside, and in the water, its near walls are
  gone. The reflection is drawn from a camera under the water, not through a
  mirror; a reflection that reversed the winding would keep them.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

BARREL = r"World\Generic\PassiveDoodads\Barrel\Barrel01.m2"            # one material, flags 0
CRATE = r"World\Generic\PassiveDoodads\Crate01\Crate01.m2"              # one material, flags 0x04
PILLAR = r"World\wmo\Transports\WMO_Elevators\org_arena_pillar.wmo"     # one group, one material, flags 0
ROOM = r"World\wmo\cameron.wmo"                                          # one group, one material, flags 0

AHEAD = {"offset": [0, 0, 4], "yaw": 0, "pitch": 0}
OUTSIDE = {"offset": [-30, -12, 10], "lookAt": [0, 0, 3]}
# Just above the water, looking across it at the model: the water mirrors
# most at a grazing angle, and below the horizon is only the reflection.
REFLECTED = {"offset": [-30, 0, -0.2], "yaw": 0, "pitch": 0}
BELOW_HORIZON = range(135, 240)


def scene(models, camera, water=False):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": models, "camera": camera, "waterPlane": water, "waterTiles": 32,
        "size": [320, 240], "frames": 12, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 1, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False,
        "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source, culling=True):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    if culling:
        env.pop("WOWEE_FACE_CULLING", None)
    else:
        env["WOWEE_FACE_CULLING"] = "0"
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


def changed(a, b, rows=range(240), width=320):
    """The share of pixels in `rows` whose colour moved by more than a rounding step."""
    moved = sum(1 for y in rows for x in range(width)
                if max(abs(a[(y * width + x) * 3 + k] - b[(y * width + x) * 3 + k]) for k in range(3)) > 8)
    return moved / (len(rows) * width)


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    failures = []

    def both(name, models, camera, water=False):
        source = scene(models, camera, water)
        return render(binary, scratch, name, source), render(binary, scratch, f"{name}-all-faces", source, False)

    def same(name, models, camera, water=False):
        culled, kept = both(name, models, camera, water)
        share = changed(culled, kept, BELOW_HORIZON) if water else changed(culled, kept)
        if share > 0.001:
            failures.append(f"{name}: {share:.1%} of the picture changed when back faces were culled; expected none")

    def gone(name, models, camera, water=False):
        culled, kept = both(name, models, camera, water)
        share = changed(culled, kept, BELOW_HORIZON) if water else changed(culled, kept)
        if share < 0.02:
            failures.append(f"{name}: culling changed {share:.1%} of the picture; its back faces are still drawn")

    # Doodads: from inside, the one-sided barrel is not there and the
    # two-sided crate is all there is.
    empty = render(binary, scratch, "empty", scene([], AHEAD))
    barrel = [{"path": BARREL, "scale": 10}]
    share = changed(empty, render(binary, scratch, "inside-barrel", scene(barrel, AHEAD)))
    if share > 0.001:
        failures.append(f"inside-barrel: {share:.1%} of the picture is its walls; expected none")
    share = changed(empty, render(binary, scratch, "inside-crate", scene([{"path": CRATE, "scale": 10}], AHEAD)))
    if share < 0.5:
        failures.append(f"inside-crate: only {share:.1%} of the picture is its walls")
    same("outside-barrel", barrel, OUTSIDE)

    # Buildings, both ways round, and the pillar in the water.
    pillar = [{"path": PILLAR, "offset": [0, 0, 4]}]
    same("outside-pillar", pillar, OUTSIDE)
    same("reflected-pillar", [{"path": PILLAR, "offset": [0, 0, 3.5]}], REFLECTED, water=True)
    gone("inside-pillar", pillar, AHEAD)
    room = [{"path": ROOM, "scale": 4}]
    same("inside-room", room, AHEAD)
    gone("outside-room", room, OUTSIDE)
    gone("reflected-room", [{"path": ROOM, "scale": 4, "offset": [0, 0, 0.5]}], REFLECTED, water=True)

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        print("see", scratch)
        return 1
    print("PASS: one-sided materials show their front faces only, in the view and the reflection")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as exit_:
        if exit_.code == 77:
            print("SKIP: no Vulkan device or game assets")
        raise
