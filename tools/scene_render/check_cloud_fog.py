#!/usr/bin/env python3
"""Moving clouds must change the sky without painting fog on opaque buildings.

The fixture places opaque WMO walls under Durotar's afternoon sky. This is
an isolated occlusion test, not a reconstruction of a Durotar world view.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

from check_sharp_stars import rgb_pixels


def difference(a, b, width, rectangle):
    left, top, right, bottom = rectangle
    values = [abs(a[3 * (y * width + x) + c] - b[3 * (y * width + x) + c])
              for y in range(top, bottom) for x in range(left, right) for c in range(3)]
    return {"mean": sum(values) / len(values), "maximum": max(values)}


def check(scratch, prefix):
    passed = True
    for mode in ("legacy", "physical"):
        paths = [scratch / f"{prefix}-{mode}-{phase}.png" for phase in (0, 3)]
        for path in paths:
            if struct.unpack_from(">II", path.read_bytes(), 16) != (1280, 720):
                raise ValueError(f"Unexpected capture dimensions: {path}")
        a, b = map(rgb_pixels, paths)
        sky = difference(a, b, 1280, (650, 10, 1000, 230))
        # Interior patches, away from MSAA silhouettes and authored transparency.
        walls = [difference(a, b, 1280, rectangle) for rectangle in (
            (550, 270, 575, 320), (597, 310, 600, 335))]
        print(mode, json.dumps({"sky": sky, "opaque_walls": walls}))
        if sky["mean"] < 2:
            print("FAIL: cloud motion did not visibly change the open sky")
            passed = False
        if any(wall["maximum"] != 0 for wall in walls):
            print("FAIL: cloud motion changed fog on an opaque wall")
            passed = False
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--prefix", default="capture")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    if not args.check_only:
        binary = args.binary.resolve()
        scene = json.loads((Path(__file__).parent / "scenes/cloud_fog_wall.json").read_text())
        env = os.environ.copy()
        env.update(WOWEE_CONFIG_ROOT=str(scratch / "profile"),
                   WOWEE_RESOURCE_ROOT=str(binary.parent), WOWEE_RENDER_THREAD="0")
        for mode in ("legacy", "physical"):
            scene["aerialRayMarching"] = mode == "physical"
            for phase in (0, 3):
                scene["cloudProbe"]["phase"] = phase
                name = f"{args.prefix}-{mode}-{phase}"
                source = scratch / f"{name}.json"
                source.write_text(json.dumps(scene), encoding="utf-8")
                run = subprocess.run([str(binary), str(source), str(scratch / f"{name}.png")],
                                     cwd=binary.parent, env=env, capture_output=True)
                output = run.stdout + run.stderr
                (scratch / f"{name}.log").write_bytes(output)
                if run.returncode or b"VUID-" in output or b"[ERROR] Vulkan:" in output:
                    print("FAIL: rendering failed; see", scratch / f"{name}.log")
                    return 1
    if not check(scratch, args.prefix):
        return 1
    print("PASS: moving clouds remain behind opaque walls in both fog modes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
