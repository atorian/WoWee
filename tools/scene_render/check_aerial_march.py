#!/usr/bin/env python3
"""Check the runtime fog switch through scene_render captures.

Use a static fixture (citadel_evening.json). Render six scenes with the same
SPIR-V: off (default), on, and mutations after frame extraction. Mutate at
the last frame for held_off/held_on, or frame 0 for next_on/next_off.
Usage: check_aerial_march.py <directory containing those six PNGs>
"""
from pathlib import Path
import sys
from check_sharp_stars import rgb_pixels


def check(directory: Path) -> int:
    images = {name: rgb_pixels(directory / f"{name}.png") for name in
              ("off", "on", "held_off", "held_on", "next_off", "next_on")}
    if len({len(pixels) for pixels in images.values()}) != 1:
        print("FAIL: capture sizes differ")
        return 1
    if images["off"] == images["on"]:
        print("FAIL: enabling ray marching does not change distant surfaces")
        return 1
    for actual, expected in (("held_off", "off"), ("held_on", "on"),
                             ("next_off", "off"), ("next_on", "on")):
        if images[actual] != images[expected]:
            print(f"FAIL: {actual} does not match {expected}")
            return 1
    print("PASS: runtime switch changes the image, preserves extracted frames, "
          "and takes effect in both directions on subsequent frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(check(Path(sys.argv[1])))
