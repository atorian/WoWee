#!/usr/bin/env python3
"""RT residency follows distance independently of raster visibility."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
scratch = Path(sys.argv[2]).resolve()
scratch.mkdir(parents=True, exist_ok=True)
scene = {"map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12,
         "origin": [5818, 405, 650], "streamWorld": False, "frames": 40,
         "camera": {"offset": [-25, -25, 18], "lookAt": [0, 0, 0]},
         "size": [320, 240], "rayTracedLighting": 3, "rtDistance": 100,
         "models": [{"path": r"World\wmo\transports\transport_ship\transportship.wmo"},
                    {"path": r"World\wmo\transports\transport_ship\transportship.wmo",
                     "offset": [4000, 4000, 0]}]}
env = os.environ.copy()
env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
source = scratch / "scene.json"
source.write_text(json.dumps(scene), encoding="utf-8")
run = subprocess.run([str(binary), str(source), str(scratch / "scene.png")],
                     cwd=binary.parent, env=env, capture_output=True)
output = run.stdout + run.stderr
(scratch / "scene.log").write_bytes(output)
if b"no Vulkan device" in output or b"no assets under" in output:
    sys.exit(77)
checked = b"\n".join(line for line in output.splitlines() if not (
    b"loader_get_json: Failed to open JSON file " in line and
    line.endswith((b"Overlay\\EOSOverlayVkLayer-Win32.json", b"Overlay\\EOSOverlayVkLayer-Win64.json"))))
if b"VUID-" in checked or b"[ERROR] Vulkan:" in checked:
    print("FAIL: Vulkan validation error; see", scratch / "scene.log")
    sys.exit(1)
if run.returncode:
    print(output.decode(errors="replace")[-3000:])
    sys.exit(1)
m = re.search(rb"RT scene (\d+) meshes (\d+) instances", output)
if not m or int(m[2]) != 1:
    print("FAIL: nearby placement must remain and the remote placement must leave RT:",
          m[0].decode() if m else "missing statistics")
    sys.exit(1)
print("PASS: RT distance keeps one nearby placement")
