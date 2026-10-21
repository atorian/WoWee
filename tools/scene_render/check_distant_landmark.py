#!/usr/bin/env python3
"""Check the low-camera Hyjal fixture: the crown survives dense M2 recording.

The fixture disables ordinary distance fog so this checks missing geometry,
independently of whether the zone's fog intentionally obscures the landmark.
"""
from pathlib import Path
import argparse
import struct
from check_sharp_stars import rgb_pixels

def check(image: Path, log: Path, region=(585,150,685,210), background: Path | None = None) -> int:
    text=log.read_text(encoding='utf-8-sig', errors='replace')
    if 'instance buffer full' in text:
        print('FAIL: recording exhausted its instance storage and dropped scene models')
        return 1
    if not any('M2 DRAW' in line and 'THEWORLDTREE' in line and '  drawn' in line for line in text.splitlines()):
        print('FAIL: the far landmark never reached a mesh draw')
        return 1
    selections=[line for line in text.splitlines() if 'M2 SELECTION' in line and 'THEWORLDTREE' in line]
    if not selections or 'selected=1 ' not in selections[-1]:
        print('FAIL: the landmark disappeared after the scene settled')
        return 1
    width,height=struct.unpack('>II',image.read_bytes()[16:24])
    x0,y0,x1,y1=region
    if not (0<=x0<x1<=width and 0<=y0<y1<=height):
        print('FAIL: canopy region lies outside the image')
        return 1
    rgb=rgb_pixels(image)
    sky=None
    if background is not None:
        if struct.unpack('>II',background.read_bytes()[16:24]) != (width,height):
            print('FAIL: background and scene dimensions differ')
            return 1
        sky=rgb_pixels(background)
    # Above the ridge; the default region is for hyjal_platform.json.
    # The sky and rock are red/brown here; the visible canopy contributes green.
    greens=0
    for y in range(y0,y1):
        for x in range(x0,x1):
            r,g,b=rgb[(y*width+x)*3:(y*width+x)*3+3]
            # A dawn sky can itself be green. Against a saved tree-free frame,
            # require actual silhouette contrast as well as canopy colour.
            darkens=sky is None or sum(sky[(y*width+x)*3:(y*width+x)*3+3]) > r+g+b+30
            greens += darkens and g>r and g>b*1.15 and g>40
    if greens<8:
        print('FAIL: no visible canopy above the ridge:',greens,'green pixels')
        return 1
    print('PASS: dense low-camera scene keeps the landmark visible:',greens,'canopy pixels')
    return 0
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('log',type=Path)
    parser.add_argument('--region',type=int,nargs=4,default=(585,150,685,210),
                        metavar=('X0','Y0','X1','Y1'))
    parser.add_argument('--background',type=Path,help='matched tree-free capture for dawn skies')
    args=parser.parse_args()
    raise SystemExit(check(args.image,args.log,args.region,args.background))
