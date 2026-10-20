#!/usr/bin/env python3
"""Render from an extracted frame, then mutate its sources and compare pixels.

Exercises the production Renderer and post-process/RT paths on the GPU. PNGs
are written by the same deterministic encoder, without timestamp metadata.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--mode", type=int, default=3)
    parser.add_argument("--fxaa", action="store_true")
    parser.add_argument("--fsr2", action="store_true")
    parser.add_argument("--msaa", type=int, default=1)
    parser.add_argument("--weather", choices=("clear", "rain"), default="clear")
    args = parser.parse_args()
    binary = args.binary.resolve()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    scene = {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": [{"path": r"World\Azeroth\Elwynn\PassiveDoodads\LampPost\LampPost.m2"}],
        "size": [640, 480], "frames": 30, "rayTracedLighting": args.mode,
        "volumetricFog": 2, "volumetricDensity": .65, "shadowQuality": 1,
        "fxaa": args.fxaa, "fsr2": args.fsr2, "fsrScale": .75 if args.fsr2 else 1,
        "msaa": args.msaa,
        "weather": args.weather, "weatherIntensity": 1 if args.weather == "rain" else 0,
        "wind": {"direction": 225, "speed": 4},
    }
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    env["WOWEE_LOG_LEVEL"] = "info"
    env.pop("WOWEE_RT_FORCE_SOFTWARE", None)
    for label, mutate in (("baseline", False), ("mutated", True), ("next-frame", True)):
        scene["mutateAfterExtract"] = mutate
        scene["mutateAtFrame"] = scene["frames"] - (2 if label == "next-frame" else 1)
        source = scratch / f"{label}.json"
        source.write_text(json.dumps(scene), encoding="utf-8")
        run = subprocess.run([str(binary), str(source), str(scratch / f"{label}.png")],
                             cwd=binary.parent, env=env, capture_output=True)
        output = run.stdout + run.stderr
        (scratch / f"{label}.log").write_bytes(output)
        if b"no Vulkan device" in output or b"no assets under" in output:
            print("SKIP: no Vulkan device or game assets")
            return 77
        if run.returncode:
            print(output.decode(errors="replace")[-4000:])
            return 1
        # The machine has two stale implicit-layer registry entries. These
        # loader diagnostics precede device creation; keep every other Vulkan
        # error fatal, including validation messages without a printed VUID.
        checked = b"\n".join(line for line in output.splitlines() if not (
            b"loader_get_json: Failed to open JSON file " in line and
            line.endswith((b"Overlay\\EOSOverlayVkLayer-Win32.json",
                           b"Overlay\\EOSOverlayVkLayer-Win64.json"))))
        if (b"VUID-" in checked or b"Validation Error:" in checked or
                b"[ERROR] Vulkan:" in checked):
            print("FAIL: Vulkan validation error; see", scratch / f"{label}.log")
            return 1
    if (scratch / "baseline.png").read_bytes() != (scratch / "mutated.png").read_bytes():
        print("FAIL: changes after extraction changed the rendered image; see", scratch)
        return 1
    if (scratch / "baseline.png").read_bytes() == (scratch / "next-frame.png").read_bytes():
        print("FAIL: the next extracted frame ignored source changes; see", scratch)
        return 1
    print("PASS: extracted camera, light, weather, settings and RT survive source mutation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
