#!/usr/bin/env python3
"""Check height influence in matched, clear-sky Hyjal captures.

Render hyjal_barrens_haze.json with fogHeightInfluence 0, 0.5 and 1;
render a matching sky with models=[]; render 1 again with
fogHeightInfluenceAfterExtract=0. Keep camera, hour and playerPosition fixed.
Usage: check_fog_height.py zero.png half.png normal.png sky.png held.png
"""
from pathlib import Path
import sys

from check_sharp_stars import rgb_pixels


def check(zero: Path, half: Path, normal: Path, sky: Path, held: Path) -> int:
    if zero.read_bytes() == normal.read_bytes():
        print("FAIL: height influence does not change the rendered landmark")
        return 1
    if normal.read_bytes() != held.read_bytes():
        print("FAIL: changing height influence changed an already extracted frame")
        return 1
    background = rgb_pixels(sky)
    if len(background) != 1280 * 720 * 3:
        print("FAIL: expected a 1280x720 Hyjal fixture")
        return 1
    contrasts = []
    for path in (zero, half, normal):
        pixels = rgb_pixels(path)
        if len(pixels) != len(background):
            print("FAIL: capture sizes differ")
            return 1
        # The crown above the horizon. A matched sky cancels its gradient;
        # denser high-altitude air must reduce this silhouette's contrast.
        contrast = sum(
            abs(pixels[i + c] - background[i + c])
            for y in range(195, 240) for x in range(600, 680)
            for i in ((y * 1280 + x) * 3,) for c in range(3)
        )
        contrasts.append(contrast)
    print("Canopy contrast at influence 0 / 0.5 / 1:", *contrasts)
    if not 0 < contrasts[0] < contrasts[1] < contrasts[2]:
        print("FAIL: reducing height influence must strengthen high-altitude haze")
        return 1
    print("PASS: visible density response and immutable frame setting")
    return 0


if __name__ == "__main__":
    raise SystemExit(check(*(Path(p) for p in sys.argv[1:])))
