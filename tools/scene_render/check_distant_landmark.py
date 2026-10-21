#!/usr/bin/env python3
"""Check the low-camera Hyjal fixture: the crown survives dense M2 recording.

The fixture disables ordinary distance fog so this checks missing geometry,
independently of whether the zone's fog intentionally obscures the landmark.
"""
from pathlib import Path
import sys
from check_sharp_stars import rgb_pixels

def check(image: Path, log: Path) -> int:
    text=log.read_text(encoding='utf-8-sig', errors='replace')
    if 'instance buffer full' in text:
        print('FAIL: recording exhausted its instance storage and dropped scene models')
        return 1
    if not any('M2 DRAW' in line and 'THEWORLDTREE' in line and '  drawn' in line for line in text.splitlines()):
        print('FAIL: the far landmark never reached a mesh draw')
        return 1
    rgb=rgb_pixels(image)
    # Above the ridge, projected from the authored tree placement in hyjal_platform.json.
    # The sky and rock are red/brown here; the visible canopy contributes green.
    greens=0
    for y in range(150,210):
        for x in range(585,685):
            r,g,b=rgb[(y*1280+x)*3:(y*1280+x)*3+3]
            greens += g>r and g>b*1.15 and g>40
    if greens<8:
        print('FAIL: no visible canopy above the ridge:',greens,'green pixels')
        return 1
    print('PASS: dense low-camera scene keeps the landmark visible:',greens,'canopy pixels')
    return 0
if __name__=='__main__':
    raise SystemExit(check(*(Path(p) for p in sys.argv[1:3])))
