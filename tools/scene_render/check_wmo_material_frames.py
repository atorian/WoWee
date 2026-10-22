#!/usr/bin/env python3
"""Settings affect newly shown WMOs but never an already extracted frame."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--msaa", type=int, default=1)
    parser.add_argument("--reflection", action="store_true")
    args = parser.parse_args()
    binary, scratch = args.binary.resolve(), args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    base = {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": [{"path": r"World\wmo\transports\transport_ship\transportship.wmo"}],
        "size": [640, 480], "frames": 8, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "fxaa": False, "msaa": args.msaa,
    }
    if args.reflection:
        base["waterPlane"] = True
        base["waterTiles"] = 32
        base["camera"] = {"offset": [-60, -60, 40], "lookAt": [0, 0, 4]}
    change = {"frame": 1, "normalMapping": False, "pom": False}
    scenarios = {}
    scenarios["original"] = copy.deepcopy(base)
    for name in ("visible", "late-visible", "queued", "next-frame"):
        scene = copy.deepcopy(base)
        event = dict(change)
        if name == "late-visible":
            scene["models"][0].update(hidden=True, shownAtFrame=2)
        if name in ("queued", "next-frame"):
            event.update(frame=7 if name == "queued" else 6, afterExtract=True)
        scene["wmoMaterialSettings"] = [event]
        scenarios[name] = scene
    env = os.environ.copy()
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    env["WOWEE_RENDER_THREAD"] = "1"
    env["WOWEE_LOG_LEVEL"] = "info"
    for name, scene in scenarios.items():
        source = scratch / (name + ".json")
        source.write_text(json.dumps(scene), encoding="utf-8")
        run = subprocess.run([str(binary), str(source), str(scratch / (name + ".png"))],
                             cwd=binary.parent, env=env, capture_output=True)
        log = run.stdout + run.stderr
        (scratch / (name + ".log")).write_bytes(log)
        checked = b"\n".join(line for line in log.splitlines() if not (
            b"loader_get_json: Failed to open JSON file " in line and
            line.endswith((b"Overlay\\EOSOverlayVkLayer-Win32.json",
                           b"Overlay\\EOSOverlayVkLayer-Win64.json"))))
        if (run.returncode or b"VUID-" in checked or b"SYNC-HAZARD" in checked or
                b"Validation Error:" in checked or b"[ERROR] Vulkan:" in checked):
            raise RuntimeError(f"{name}: renderer/validation failed; inspect {scratch / (name + '.log')}")
    pixels = {name: (scratch / (name + ".png")).read_bytes() for name in scenarios}
    if pixels["original"] == pixels["visible"]:
        raise RuntimeError("fixture did not exercise a visible material setting")
    if pixels["visible"] != pixels["late-visible"]:
        raise RuntimeError("WMO shown after settings changed retained old material settings")
    if pixels["original"] != pixels["queued"]:
        raise RuntimeError("settings changed an already extracted frame")
    if pixels["visible"] != pixels["next-frame"]:
        raise RuntimeError("next extraction did not apply changed settings")
    print("PASS: hidden WMO, queued snapshot and next-frame material settings")


if __name__ == "__main__":
    main()
