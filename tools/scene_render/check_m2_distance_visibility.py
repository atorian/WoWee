#!/usr/bin/env python3
"""Reject structured holes inside the distant world-tree canopy.

Use hyjal_barrens_haze.json, the unchanged camera, 1280x720 and View Distance
10000. This interior crown patch excludes the natural silhouette. A Bayer
retirement mask exposed sky in one modulo-4 phase while neighbouring pixels
still drew leaves. Test the rendered image, independently of shader layout.
"""
from pathlib import Path
import struct
import sys
from check_sharp_stars import rgb_pixels


def check(path):
    if struct.unpack('>II', path.read_bytes()[16:24]) != (1280, 720):
        raise AssertionError('expected the 1280x720 Hyjal fixture')
    pixels = rgb_pixels(path)
    phases = [[] for _ in range(16)]
    for y in range(214, 226):
        for x in range(628, 652):
            phases[(x % 4) + (y % 4)*4].append(sum(pixels[(y*1280+x)*3:(y*1280+x)*3+3]))
    means = [sum(p)/len(p) for p in phases]
    sky = [sum(pixels[(y*1280+710)*3:(y*1280+710)*3+3]) for y in range(214, 226)]
    if sum(sky)/len(sky) - sum(means)/len(means) < 20:
        raise AssertionError('canopy missing or indistinguishable from sky')
    spread = max(means)-min(means)
    if spread > 35:
        raise AssertionError(f'periodic sky holes in canopy: phase luminance spread {spread:.2f} > 35')
    print(f'PASS: no structured canopy holes (phase spread {spread:.2f})')


def check_boundary(directory):
    for samples in (1, 4):
        background = rgb_pixels(directory / f'boundary-{samples}-5900.png')
        # Setup frames can submit the model before the final view is applied;
        # inspect the saved frame, not the one-time startup draw diagnostics.
        for y in range(196, 275):
            sky = background[(y*1280+710)*3:(y*1280+710)*3+3]
            for x in range(610, 670):
                i = (y*1280+x)*3
                if max(abs(background[i+c]-sky[c]) for c in range(3)) > 2:
                    raise AssertionError('model remains visible beyond its cull range')
        contrasts = []
        full = rgb_pixels(directory / f'boundary-{samples}-8000.png')
        for limit in (8000, 6500, 6200):
            pixels = rgb_pixels(directory / f'boundary-{samples}-{limit}.png')
            if pixels != full:
                raise AssertionError(f'MSAA {samples}: model changes before cull range {limit}')
            contrast = 0
            for y in range(196, 275):
                for x in range(610, 670):
                    i = (y*1280+x)*3
                    contrast += sum(abs(pixels[i+c]-background[i+c]) for c in range(3))
            contrasts.append(contrast)
        if min(contrasts) <= 100:
            raise AssertionError(f'MSAA {samples}: visible model missing before cutoff')
        print(f'PASS: MSAA {samples} silhouette unchanged before cutoff; contrast {contrasts[0]}')


if __name__ == '__main__':
    check(Path(sys.argv[1]))
    if len(sys.argv) > 2:
        check_boundary(Path(sys.argv[2]))
