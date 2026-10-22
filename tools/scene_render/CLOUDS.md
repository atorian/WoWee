# Offline cloud regression

Run from PowerShell on D:, without launching the game:

```powershell
.\tools\scene_render\clouds.ps1
```

The script builds only `scene_render` and its celestial behaviour test, then
renders twelve fixed sky captures. `-BuildDir` can select an existing build
tree; its sources must contain the changes being evaluated.

Each run saves JSON scenes, PNGs, logs, measured coverage, the cloud shader
hash, and an `index.html` gallery under `build-dev/scratch/cloud-tests`.
The next run compares against the saved latest metrics without rendering the
old version again. Use `-Baseline <directory>` to select another saved run.

Inspect the day/night upward and horizon views, Durotar, sunset and animation
phase captures. The synthetic white-cloud fixtures check clear-sky gaps,
broad soft transitions with visible interiors, overcast coverage, zero-density disable equivalence and
continuity under a small animation step. Their measurements cancel the
matching cloud-free underlay and operate in display space; they are not
linear optical-depth measurements. Passing these checks does not establish
visual parity with the original client.

`cloudProbe` is an offline fixture input: `phase` freezes animation, `density`
optionally overrides coverage, and `mask` selects the diagnostic palette.
All captures use the renderer's actual cloud pass.
