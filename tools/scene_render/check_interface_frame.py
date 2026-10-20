#!/usr/bin/env python3
"""An interface frame survives source mutation, atlas growth and context destruction."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
scratch.mkdir(parents=True, exist_ok=True)
scene = {"map": "Northrend", "mapId": 571, "hour": 12, "streamWorld": False,
         "origin": [5800, 600, 650], "frames": 6, "size": [480, 320],
         "interface": True, "rayTracedLighting": 0, "volumetricFog": 0}
env = os.environ.copy()
env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
for name in ("baseline", "mutated", "destroyed", "next-frame"):
    scene["mutateInterfaceAfterExtract"] = name in ("mutated", "next-frame")
    scene["destroyInterfaceAfterExtract"] = name == "destroyed"
    scene["mutateAtFrame"] = scene["frames"] - (2 if name == "next-frame" else 1)
    source = scratch / f"{name}.json"
    source.write_text(json.dumps(scene), encoding="utf-8")
    run = subprocess.run([str(binary), str(source), str(scratch / f"{name}.png")],
                         cwd=binary.parent, env=env, capture_output=True)
    output = run.stdout + run.stderr
    (scratch / f"{name}.log").write_bytes(output)
    if b"no Vulkan device" in output or b"no assets under" in output:
        sys.exit(77)
    checked = b"\n".join(line for line in output.splitlines() if not (
        b"loader_get_json: Failed to open JSON file " in line and
        line.endswith((b"Overlay\\EOSOverlayVkLayer-Win32.json", b"Overlay\\EOSOverlayVkLayer-Win64.json"))))
    if run.returncode or b"VUID-" in checked or b"[ERROR] Vulkan:" in checked:
        print("FAIL: interface capture/validation failed; see", scratch / f"{name}.log")
        print(output.decode(errors="replace")[-2400:])
        sys.exit(1)
baseline = (scratch / "baseline.png").read_bytes()
for name in ("mutated", "destroyed"):
    if baseline != (scratch / f"{name}.png").read_bytes():
        print("FAIL:", name, "interface source changed the held frame")
        sys.exit(1)
if baseline == (scratch / "next-frame.png").read_bytes():
    print("FAIL: next extraction ignored the changed interface")
    sys.exit(1)
print("PASS: frozen interface, grown fonts, destroyed context, clipping and large vertex offsets")
