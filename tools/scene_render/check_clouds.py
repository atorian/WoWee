#!/usr/bin/env python3
"""Render the client's cloud pass without the game; save images and compare metrics.

Usage: check_clouds.py path/to/scene_render.exe output-dir [--baseline old-dir]
Requires the normal scene_render data/runtime environment, no Python packages.
The output directory must be new so the previous run remains reviewable.
"""
import argparse
import copy
import html
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels


def fixtures():
    base = {
        "map": "Kalimdor", "mapId": 1, "zoneId": 17, "hour": 12,
        "origin": [-440.732, -3633.28, 102.297],
        "playerPosition": [-440.732, -3633.28, 102.297],
        "streamWorld": False, "models": [], "frames": 40,
        "camera": {"offset": [0, 0, 5], "yaw": 1.18, "pitch": 65},
        "size": [1280, 720], "viewDistance": 10000,
        "msaa": 4, "bloom": 0, "fxaa": False, "fsr2": False,
        "volumetricFog": 0, "shadowQuality": 0, "rayTracedLighting": 0,
        "lens": False, "sunShafts": False, "fogStrength": 0.8,
        "cloudProbe": {"phase": 0.0},
    }
    cases = {}
    for name, hour, pitch in [("day_up", 12, 65), ("day_horizon", 12, 8),
                              ("night_up", 0, 65), ("night_horizon", 0, 8),
                              ("sunset", 19, 30)]:
        scene = copy.deepcopy(base)
        scene["hour"], scene["camera"]["pitch"] = hour, pitch
        cases[name] = scene
    cases["day_later"] = copy.deepcopy(base)
    cases["day_later"]["cloudProbe"]["phase"] = 3.0
    cases["durotar"] = copy.deepcopy(base)
    cases["durotar"]["origin"] = cases["durotar"]["playerPosition"] = [1174.47, -4846.67, 100]
    cases["durotar"]["zoneId"] = 14
    for name, density, phase in [("mask_clear", 0, 0), ("mask_fair", .5, 0),
                                  ("mask_overcast", .95, 0), ("mask_motion", .5, .002)]:
        scene = copy.deepcopy(base)
        scene["cloudProbe"] = {"mask": True, "density": density, "phase": phase}
        cases[name] = scene
    cases["mask_disabled"] = copy.deepcopy(cases["mask_fair"])
    cases["mask_disabled"]["clouds"] = False
    return cases


def metrics(folder):
    masks = {n: rgb_pixels(folder / (n + ".png"))[::3]
             for n in ("mask_clear", "mask_fair", "mask_overcast", "mask_motion")}
    # Cancel the matched cloud-free underlay (including the sun's diffuse
    # halo). This is display-space contrast, not a claim of linear optical depth.
    background = masks["mask_clear"]
    for name in ("mask_fair", "mask_overcast", "mask_motion"):
        masks[name] = [max(0, min(255, round((v-b) * 255 / max(1, 255-b))))
                       for v, b in zip(masks[name], background)]
    fair, storm = masks["mask_fair"], masks["mask_overcast"]
    count = len(fair)
    # Thresholds are in captured display values. We measure visible dense
    # cores and gaps, not copy a shader constant or demand a particular seed.
    return {
        "zero_density_matches_disabled": rgb_pixels(folder / "mask_disabled.png") == rgb_pixels(folder / "mask_clear.png"),
        "fair_gap_fraction": sum(v < 8 for v in fair) / count,
        "fair_core_fraction": sum(v > 235 for v in fair) / count,
        "fair_translucent_fraction": sum(8 <= v <= 235 for v in fair) / count,
        "fair_peak_contrast": max(fair),
        "overcast_core_fraction": sum(v > 235 for v in storm) / count,
        "motion_mean_change": sum(abs(a-b) for a, b in zip(fair, masks["mask_motion"])) / count,
        "weather_decreased_pixels": sum(b + 1 < a for a, b in zip(fair, storm)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    if output.exists():
        parser.error("output already exists; choose a new run directory")
    output.mkdir(parents=True)
    env = os.environ.copy()
    env.update(WOWEE_CONFIG_ROOT=str(output / "profile"),
               WOWEE_RESOURCE_ROOT=str(binary.parent), WOWEE_LOG_LEVEL="info")
    for name in ("WOWEE_SCENE_REPLAY", "WOWEE_SCENE_CAPTURE", "WOWEE_PERF_TRACE", "WOWEE_BENCHMARK"):
        env.pop(name, None)
    cases = fixtures()
    for name, scene in cases.items():
        source = output / (name + ".json")
        source.write_text(json.dumps(scene, indent=2), encoding="utf-8")
        with (output / (name + ".log")).open("wb") as log:
            result = subprocess.run([str(binary), str(source), str(output / (name + ".png"))],
                                    cwd=binary.parent, env=env, stdout=log, stderr=log)
        if result.returncode:
            print("FAIL:", name, "see", output / (name + ".log"))
            return 1
        print("Rendered", name, flush=True)
    result = metrics(output)
    failures = []
    if not result["zero_density_matches_disabled"]: failures.append("zero density does not match disabled clouds")
    if not .15 < result["fair_gap_fraction"] < .85: failures.append("fair weather must leave substantial sky gaps")
    if result["fair_translucent_fraction"] < 2 * result["fair_core_fraction"]:
        failures.append("fair clouds have hard dense silhouettes instead of broad soft transitions")
    if result["fair_peak_contrast"] < 220: failures.append("soft clouds lost their visible interiors")
    if result["overcast_core_fraction"] < .75: failures.append("overcast does not close the sky")
    if result["weather_decreased_pixels"]: failures.append("increasing cloud cover removes existing clouds")
    if not 0 < result["motion_mean_change"] < 2: failures.append("small animation step is static or discontinuous")
    report = {"metrics": result, "failures": failures,
              "shader_sha256": hashlib.sha256((binary.parent / "assets/shaders/clouds.frag.spv").read_bytes()).hexdigest(),
              "note": "Numeric checks do not replace visual review of the colour captures."}
    if args.baseline:
        previous = json.loads((args.baseline / "results.json").read_text())["metrics"]
        report["change_from_baseline"] = {k: result[k] - previous[k] for k in result
                                          if k in previous and not isinstance(result[k], bool)}
    (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    cards = "".join('<figure><img src="' + n + '.png"><figcaption>' + html.escape(n) + '</figcaption></figure>' for n in cases)
    (output / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>Cloud render test</title>'
        '<style>body{background:#202329;color:#eee;font:16px sans-serif}main{display:grid;grid-template-columns:1fr 1fr;gap:12px}'
        'figure{margin:0}img{width:100%}figcaption{padding:8px}pre{white-space:pre-wrap}</style>'
        '<h1>Cloud render test</h1><pre>' + html.escape(json.dumps(report, indent=2)) + '</pre><main>' + cards + '</main>', encoding="utf-8")
    print(json.dumps(report, indent=2))
    print("FAIL" if failures else "PASS", output / "index.html")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
