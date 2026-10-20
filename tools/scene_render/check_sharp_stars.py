#!/usr/bin/env python3
"""The sharp-star setting changes a real sky, using the extracted frame's value."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import zlib


def rgb_pixels(path: Path) -> bytes:
    """Read the RGB/RGBA8, noninterlaced PNGs written by scene_render's stb encoder."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    offset, compressed = 8, bytearray()
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind, chunk = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlaced = struct.unpack(">IIBBBBB", chunk)
            assert depth == 8 and colour in (2, 6) and not interlaced
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            compressed.extend(chunk)
        offset += length + 12
    raw = zlib.decompress(compressed)
    stride, result = width * channels, bytearray()
    previous = bytearray(stride)
    for y in range(height):
        row_at = y * (stride + 1)
        filter_kind = raw[row_at]
        row = bytearray(raw[row_at + 1:row_at + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            above, corner = previous[x], previous[x - channels] if x >= channels else 0
            if filter_kind == 1:
                prediction = left
            elif filter_kind == 2:
                prediction = above
            elif filter_kind == 3:
                prediction = (left + above) // 2
            elif filter_kind == 4:
                base = left + above - corner
                prediction = min((left, above, corner), key=lambda v: abs(base - v))
            else:
                assert filter_kind == 0
                prediction = 0
            row[x] = (row[x] + prediction) & 255
        for x in range(0, stride, channels):
            result.extend(row[x:x + 3])
        previous = row
    return bytes(result)


def check_captures(on: Path, off: Path, mutated: Path) -> bool:
    if on.read_bytes() == off.read_bytes():
        print("FAIL: switching sharp stars did not change the night sky")
        return False
    if on.read_bytes() != mutated.read_bytes():
        print("FAIL: changing sharp stars after extraction changed the held frame")
        return False
    sharp, baked = rgb_pixels(on), rgb_pixels(off)
    # Removing the old sheets alone makes a different picture but leaves a
    # starless sky. Replacement must both remove old light and add new points.
    brighter = darker = 0
    for p in range(0, len(sharp), 3):
        delta = sum(sharp[p:p + 3]) - sum(baked[p:p + 3])
        brighter += delta > 12
        darker += delta < -12
    if not brighter or not darker:
        print("FAIL: star replacement must add point light and remove baked light:", brighter, darker)
        return False
    print("Sky pixels brighter/darker after replacement:", brighter, darker)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--fxaa", action="store_true")
    parser.add_argument("--msaa", type=int, default=1)
    args = parser.parse_args()
    binary = args.binary.resolve()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    scene = {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 0,
        "server": [5804, 624, 647], "streamWorld": False,
        "camera": {"offset": [0, 0, 50], "yaw": 0, "pitch": 65},
        "fov": 45, "size": [1280, 720], "frames": 90,
        "rayTracedLighting": 0, "volumetricFog": 0, "sunShafts": False,
        "fxaa": args.fxaa, "msaa": args.msaa,
    }
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    env["WOWEE_LOG_LEVEL"] = "info"
    for name in ("on", "off", "mutated"):
        scene["sharpStars"] = name != "off"
        scene["mutateAfterExtract"] = name == "mutated"
        source = scratch / f"{name}.json"
        source.write_text(json.dumps(scene), encoding="utf-8")
        run = subprocess.run([str(binary), str(source), str(scratch / f"{name}.png")],
                             cwd=binary.parent, env=env, capture_output=True)
        output = run.stdout + run.stderr
        (scratch / f"{name}.log").write_bytes(output)
        if b"no Vulkan device" in output or b"no assets under" in output:
            return 77
        checked = b"\n".join(line for line in output.splitlines() if not (
            b"loader_get_json: Failed to open JSON file " in line and
            line.endswith((b"Overlay\\EOSOverlayVkLayer-Win32.json",
                           b"Overlay\\EOSOverlayVkLayer-Win64.json"))))
        if run.returncode or b"VUID-" in checked or b"[ERROR] Vulkan:" in checked:
            print("FAIL: sky capture or Vulkan validation failed; see", scratch / f"{name}.log")
            return 1
        if b"Skybox model loaded:" not in output:
            print("FAIL: fixture did not draw an authored sky model")
            return 1
    if not check_captures(scratch / "on.png", scratch / "off.png", scratch / "mutated.png"):
        return 1
    print("PASS: sharp stars replace the night sky's star sheets and survive source mutation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
