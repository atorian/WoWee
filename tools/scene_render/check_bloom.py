#!/usr/bin/env python3
"""Draw a sunlit sea: bloom adds a halo and respects extracted frame settings."""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels


def main() -> int:
    binary, scratch = (Path(arg).resolve() for arg in sys.argv[1:3])
    scratch.mkdir(parents=True, exist_ok=True)
    scene = {
        "map": "Azeroth", "mapId": 0, "zoneId": 33, "hour": 20.3,
        "server": [-14600, 1400, 2], "streamWorld": False,
        "waterPlane": True, "waterTiles": 64,
        "camera": {"offset": [-12, -12, 1], "yaw": 45, "pitch": -2},
        "size": [641, 361], "frames": 30,
        "sunShafts": False, "volumetricFog": 0, "rayTracedLighting": 0,
    }
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_LOG_LEVEL"] = "info"
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    for name in ("off", "on", "mutated"):
        scene["bloom"] = 0.0 if name == "off" else 0.8
        scene["mutateAfterExtract"] = name == "mutated"
        source = scratch / f"{name}.json"
        source.write_text(json.dumps(scene), encoding="utf-8")
        run = subprocess.run([str(binary), str(source), str(scratch / f"{name}.png")],
                             cwd=binary.parent, env=env, capture_output=True)
        output = run.stdout + run.stderr
        (scratch / f"{name}.log").write_bytes(output)
        if b"no Vulkan device" in output or b"no assets under" in output:
            return 77
        if run.returncode or b"VUID-" in output or b"[ERROR] Bloom:" in output:
            print("FAIL: bloom capture/validation failed; see", scratch / f"{name}.log")
            return 1
    on, off, held = (rgb_pixels(scratch / f"{name}.png") for name in ("on", "off", "mutated"))
    if on != held:
        print("FAIL: changing bloom after extraction changed the held frame")
        return 1
    if len(on) != len(off):
        print("FAIL: captures have different dimensions")
        return 1
    added = [b - a for a, b in zip(off, on)]
    if min(added) < -1 or max(added) < 5:
        print("FAIL: bloom must add visible light without darkening the scene", min(added), max(added))
        return 1
    # A global exposure increase also brightens the sea, but does not leave
    # the dark water far from the sun alone or spill light beside bright cores.
    halo = unchanged = 0
    for p in range(0, len(off), 3):
        if max(off[p:p + 3]) < 180:
            delta = max(added[p:p + 3])
            halo += delta >= 2
            unchanged += delta == 0
    if not halo or not unchanged:
        print("FAIL: expected local halos and unchanged dark water", halo, unchanged)
        return 1
    print("PASS: bloom adds local light, keeps dark water, and survives source mutation", halo, unchanged)
    return 0


if __name__ == "__main__":
    sys.exit(main())
