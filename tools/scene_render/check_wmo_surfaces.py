#!/usr/bin/env python3
"""Compare evaluated WMO surfaces to forward lighting on real textured geometry."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
from check_sharp_stars import rgb_pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--msaa", type=int, default=1)
    parser.add_argument("--hour", type=float, default=12)
    parser.add_argument("--parallel", action="store_true")
    parser.add_argument("--fxaa", action="store_true")
    parser.add_argument("--layers", action="store_true")
    parser.add_argument("--scene", type=Path)
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()
    binary, scratch = args.binary.resolve(), args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    scene = {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": args.hour,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": [{"path": r"World\wmo\transports\transport_ship\transportship.wmo"}],
        "size": [640, 480], "frames": 8, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "fxaa": args.fxaa, "msaa": args.msaa,
    }
    if args.scene:
        scene.update(json.loads(args.scene.read_text(encoding="utf-8-sig")))
        scene.update(size=[640,480], frames=8, rayTracedLighting=0, msaa=args.msaa,
                     fxaa=args.fxaa, hour=args.hour)
    if args.layers:
        scene.update(models=[], camera={"offset": [-8, 0, 0], "lookAt": [0, 0, 0]},
                     bloom=0, fogStrength=0, wmoLayers=[
                         {"x": 3, "rect": [-3,-3,3,3], "rgba": [180,30,20,255]},
                         {"x": 1, "rect": [-2,-2,2,2], "rgba": [20,30,210,128], "blend": 2},
                         {"x": 2, "rect": [-1.5,-1.5,1.5,1.5], "rgba": [20,210,30,255]},
                         {"x": .5, "rect": [-2,-2,0,2], "rgba": [220,180,20,255], "blend": 1, "cutout": True},
                         {"x": 0, "rect": [0,-2,2,2], "rgba": [180,20,180,96], "blend": 2},
                     ])
    if args.rebuild:
        scene["bloom"]=0
    source = scratch / "scene.json"
    source.write_text(json.dumps(scene), encoding="utf-8")
    env = os.environ.copy()
    env.update(WOWEE_RESOURCE_ROOT=str(binary.parent), WOWEE_CONFIG_ROOT=str(scratch / "config"),
               WOWEE_RENDER_THREAD="1", WOWEE_LOG_LEVEL="info")
    if args.parallel:
        env.pop("WOWEE_SINGLE_THREAD_RECORD", None)
    else:
        env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    cases=["forward", "surfaces"]
    if args.layers:
        cases.append("reordered")
    if args.rebuild:
        cases.append("rebuilt")
    original_scene=copy.deepcopy(scene)
    for name in cases:
        scene=copy.deepcopy(original_scene)
        if name == "reordered":
            scene["wmoLayers"].sort(key=lambda layer: layer.get("blend", 0) >= 2)
            source = scratch / "reordered.json"
            source.write_text(json.dumps(scene), encoding="utf-8")
        if name == "rebuilt":
            scene["msaa"]=1
            scene["fxaa"]=False
            scene["renderTargetChanges"]=[{"frame": 2, "msaa": 4, "fxaa": True},
                                           {"frame": 4, "msaa": 1, "fxaa": False},
                                           {"frame": 6, "msaa": args.msaa, "fxaa": args.fxaa}]
            source=scratch / "rebuilt.json"
            source.write_text(json.dumps(scene),encoding="utf-8")
        env["WOWEE_MODEL_SURFACES"]="1" if name in ("surfaces","rebuilt") else "0"
        env.pop("WOWEE_WMO_SURFACES", None)
        if name in ("surfaces", "rebuilt"):
            env["WOWEE_WMO_SURFACES"] = "1"
        run = subprocess.run([str(binary), str(source), str(scratch / (name + ".png"))],
                             cwd=binary.parent, env=env, capture_output=True, timeout=120)
        log = run.stdout + run.stderr
        (scratch / (name + ".log")).write_bytes(log)
        checked = b"\n".join(line for line in log.splitlines() if not (
            b"loader_get_json: Failed to open JSON file " in line and
            line.endswith((b"Overlay\\EOSOverlayVkLayer-Win32.json", b"Overlay\\EOSOverlayVkLayer-Win64.json"))))
        if run.returncode or any(error in checked for error in (
                b"VUID-", b"SYNC-HAZARD", b"Validation Error:", b"[ERROR]")):
            raise RuntimeError(f"{name}: renderer/validation failed; inspect {scratch / (name + '.log')}")
        if name == "surfaces" and b"WMO evaluated surfaces:" not in log:
            raise RuntimeError("fixture did not exercise the evaluated-surface path")
    a, b = (rgb_pixels(scratch / (name + ".png")) for name in ("forward", "surfaces"))
    if args.rebuild and b!=rgb_pixels(scratch / "rebuilt.png"):
        raise RuntimeError("recreated scene targets changed the final surface image")
    if args.layers:
        reordered=rgb_pixels(scratch / "reordered.png")
        if sum(abs(x-y)>4 for x,y in zip(a,reordered)) < len(a)*.01:
            raise RuntimeError("layer fixture does not detect moving opaque geometry across blended layers")
    if len(a) != 640 * 480 * 3 or len(b) != len(a):
        raise RuntimeError("unexpected capture dimensions")
    errors = sorted(abs(x - y) for x, y in zip(a, b))
    report = {"mean_absolute_channel_error": sum(errors) / len(errors),
              "p99_channel_error": errors[int(len(errors) * .99)],
              "max_channel_error": errors[-1],
              "channels_above_4": sum(e > 4 for e in errors) / len(errors),
              "msaa": args.msaa, "hour": args.hour, "parallel": args.parallel,
              "fxaa": args.fxaa, "layers": args.layers}
    (scratch / "comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))
    # FP16 texture/vertex colours and octahedral normals permit rounding, but
    # cannot change silhouettes, texture layout, material light or layer order.
    if report["mean_absolute_channel_error"] > .25 or report["channels_above_4"] > .001:
        raise RuntimeError("surface lighting differs visibly from forward reference")


if __name__ == "__main__":
    main()
