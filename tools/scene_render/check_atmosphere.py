#!/usr/bin/env python3
"""Check transport behaviour through static scene_render PNGs.

Use citadel_evening with aerialRayMarching=true, and patches:
  clear: no changes; thick: fogStrength=1.25;
  zero: fogStrength=0; zero_legacy: fogStrength=0,aerialRayMarching=false;
  weather_auto: weather=fog,weatherIntensity=.7,volumetricFog=0;
  weather_low: same with volumetricFog=1; high: volumetricFog=3.
The minimum volume is already Low; enabling Low again must not add another fog.
Uses the PNG reader shared with the renderer snapshot tests.
"""
import sys
import json
from pathlib import Path
from check_sharp_stars import rgb_pixels


def check(directory):
    def pixels(name):
        return rgb_pixels(directory / f'{name}.png')
    for left, right in [('zero', 'zero_legacy'), ('weather_auto', 'weather_low')]:
        if pixels(left) != pixels(right):
            raise AssertionError(f'{left} differs from {right}')
    clear, thick, high = (pixels(name) for name in ['clear', 'thick', 'high'])
    if clear == thick:
        raise AssertionError('Thickness does not change the rendered atmosphere')
    histogram = [0]*256
    for a,b in zip(clear,high): histogram[abs(a-b)] += 1
    mean = sum(i*n for i,n in enumerate(histogram))/len(clear)
    cumulative = 0
    p99 = 0
    for i,n in enumerate(histogram):
        cumulative += n
        if cumulative >= .99*len(clear):
            p99 = i
            break
    if mean > 1.0 or p99 > 3:
        raise AssertionError(f'Low/High volume discrepancy: mean {mean}, p99 {p99}')
    report = {'passed': True, 'low_high_mean_error': mean, 'low_high_p99_error': p99}
    (directory/'checks.json').write_text(json.dumps(report,indent=2))
    print(f'PASS: vacuum is neutral, weather integrated once, density responds; '
          f'Low/High error mean {mean:.4f}, p99 {p99}/255')


if __name__ == '__main__':
    check(Path(sys.argv[1]))
