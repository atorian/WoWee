#!/usr/bin/env python3
"""Ground clutter is drawn as the client lays it out, and both its settings count.

Density: the same view of Goldshire's verge - the road, a fence, the grass and
flowers by the inn - drawn at the client's lowest groundEffectDensity and at
its highest. At 16 a chunk plants a quarter of its quads at most, at 256 nearly
all of them, so the denser picture has clutter where the sparser one shows bare
ground. If the clutter is not drawn at all the two are the same picture.

Distance: a view from above over the meadow beyond the road, at the client's
default groundEffectDist of 70 yards and at its most, 140. The meadow lies
between the two, so it is bare in one and grown in the other.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import zlib


def rgb_pixels(path: Path) -> tuple[int, int, bytes]:
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
    return width, height, bytes(result)


def changed_share(first: Path, second: Path, from_row: float) -> float:
    """The share of the picture below `from_row` (a fraction of its height) that differs."""
    width, height, a = rgb_pixels(first)
    _, _, b = rgb_pixels(second)
    changed = total = 0
    for y in range(int(height * from_row), height):
        for x in range(width):
            p = (y * width + x) * 3
            total += 1
            changed += sum(abs(a[p + i] - b[p + i]) for i in range(3)) > 30
    return changed / total


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    base = {
        "map": "Azeroth", "mapId": 0, "zoneId": 12, "hour": 12.0,
        "server": [-9440.0, 70.0, 56.5], "terrain": True,
        "size": [640, 360], "frames": 30, "settleFrames": 2400,
        "rayTracedLighting": 0, "volumetricFog": 0, "sunShafts": False, "clouds": False,
    }
    verge = {"offset": [0, 0, 4], "yaw": 140, "pitch": -15}
    meadow = {"offset": [0, 0, 30], "yaw": 200, "pitch": -18}
    runs = {
        "sparse": {"camera": verge, "groundEffectDensity": 16},
        "dense": {"camera": verge, "groundEffectDensity": 256},
        "near": {"camera": meadow, "groundEffectDensity": 256, "groundEffectDist": 70.0},
        "far": {"camera": meadow, "groundEffectDensity": 256, "groundEffectDist": 140.0},
    }
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    for name, settings in runs.items():
        source = scratch / f"{name}.json"
        source.write_text(json.dumps({**base, **settings}), encoding="utf-8")
        run = subprocess.run([str(binary), str(source), str(scratch / f"{name}.png")],
                             cwd=binary.parent, env=env, capture_output=True)
        output = run.stdout + run.stderr
        (scratch / f"{name}.log").write_bytes(output)
        if b"no Vulkan device" in output or b"no assets under" in output:
            return 77
        if run.returncode or b"VUID-" in output or b"[ERROR] Vulkan:" in output:
            print("FAIL: capture or Vulkan validation failed; see", scratch / f"{name}.log")
            return 1

    ok = True
    density = changed_share(scratch / "sparse.png", scratch / "dense.png", 0.5)
    print(f"Ground pixels changed from density 16 to 256: {density:.1%}")
    if density < 0.05:
        print("FAIL: the clutter did not grow with groundEffectDensity - is it drawn at all?")
        ok = False
    distance = changed_share(scratch / "near.png", scratch / "far.png", 0.0)
    print(f"Pixels changed from distance 70 to 140: {distance:.2%}")
    if distance < 0.004:
        print("FAIL: the meadow past 70 yards did not grow clutter at groundEffectDist 140")
        ok = False
    if not ok:
        return 1
    print("PASS: ground clutter is drawn, and groundEffectDensity and groundEffectDist both reach it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
