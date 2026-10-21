#!/usr/bin/env python3
"""Check a distant citadel against the same sky and its unfogged appearance.

Render fogged_citadel.json, then with its Icecrown model hidden, then with
fogStrength=0. Keep playerPosition fixed so all three sample the same light.
Usage: check_fog_sky.py fogged.png sky.png clear.png
Only the exposed spires are checked: Dalaran in front retains its own shading.
"""
from pathlib import Path
import sys
from check_sharp_stars import rgb_pixels


def check(fogged: Path, sky: Path, clear: Path) -> int:
    a, b, c = rgb_pixels(fogged), rgb_pixels(sky), rgb_pixels(clear)
    width, height = 960, 640
    if len(a) != width * height * 3 or len(b) != len(a) or len(c) != len(a):
        print('FAIL: expected three 960x640 captures of fogged_citadel.json')
        return 1
    dot, energy = 0, 0
    for y in range(105, 245):
        for x in range(380, 450):
            i = (y * width + x) * 3
            for channel in range(3):
                unfogged = c[i+channel] - b[i+channel]
                dot += (a[i+channel] - b[i+channel]) * unfogged
                energy += unfogged * unfogged
    if energy == 0:
        print('FAIL: the clear reference contains no landmark contrast')
        return 1
    retained = dot / energy
    print(f'Distant spires retain {retained:.1%} of their unfogged contrast')
    # Positive: haze approaches the sky from the surface colour, instead of
    # replacing a dark building with a brighter-than-sky white cutout.
    # Nonzero: the old zone fog end must not erase a still-in-range landmark.
    if not .01 < retained < .5:
        print('FAIL: distant geometry vanished, lost haze, or acquired a wrong-colour cutout')
        return 1
    print('PASS: a faint landmark remains, blended toward its directional sky')
    return 0


if __name__ == '__main__':
    raise SystemExit(check(*(Path(p) for p in sys.argv[1:4])))
