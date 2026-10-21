# Distant in-scattering preview

Branch: `codex/aerial-march-preview`. The parent snapshot commit records the
uncommitted rendering work used for the comparison; the following experiment
is separate from it. No changes are installed in the normal client.

These are paired visual experiments, not a claim of a finished physical
atmosphere. Both variants use the same executable, scene, camera, weather,
view distance, materials and Fog Thickness 0.8. Procedural clouds, bloom and
the near fog volume are disabled to isolate distant in-scattering.

- `hyjal_day.json`: streamed Barrens landscape facing Hyjal at noon.
- `citadel_evening.json`: Dalaran and Icecrown silhouettes at 17:30.

## What changes

The existing visibility curve is preserved. Instead of mixing every surface
with one sky colour, `aerial_march.glsli` integrates source radiance over 24
quadratically spaced segments of the viewing ray. Each segment uses the
existing optical-depth profile, a height-dependent aerosol/molecular phase
mixture, and approximate attenuation toward the sun. The zone's sky palette
provides the light budget; DBC values are not calibrated physical irradiance.

The medium is stratified and smooth. This does not add terrain-shadowed far
shafts, cloud shadows, multiple scattering or terrain-following valley mist.
The separate existing near volume is deliberately excluded from these tests;
unifying the two media is future work. The sky itself is unchanged, and the
existing final View Distance fade still matches it. Visibility changes and
noise are not being used to disguise an illumination change.

This per-fragment integration is a reference prototype. If the appearance is
useful, move it to a low-resolution volume/LUT and measure both reconstruction
quality and GPU cost before enabling it in the ordinary client.

## Reproduce (Windows PowerShell)

Use the dev preset, with TMP/TEMP/TMPDIR on D:, and provide runtime DLLs in
`build-dev/bin`. `WOW_DATA_PATH` can point at the existing Data directory.

```powershell
./tools/scene_render/aerial_march_preview.ps1 -Scene hyjal_day -Variant before
./tools/scene_render/aerial_march_preview.ps1 -Scene hyjal_day -Variant after
./tools/scene_render/aerial_march_preview.ps1 -Scene citadel_evening -Variant before
./tools/scene_render/aerial_march_preview.ps1 -Scene citadel_evening -Variant after
./tools/scene_render/aerial_march_preview.ps1 -Scene citadel_evening -Variant reference
```

Each invocation builds `scene_render`, compiles and validates the selected
shader variant into this worktree's runtime directory, and retains a PNG and
complete log under `build-dev/scratch/aerial-march`. `reference` doubles the
integration steps to 48 for a convergence comparison. No world server or game
account is involved. The main client remains running if it was running.

## Captured results (2026-10-21)

The original five captures completed with no ERROR entries or incomplete-terrain
messages. All seven affected material shaders passed glslc and spirv-val.
The evening 24/48-step comparison differs by at most 1/255 in any channel
(mean absolute difference 0.00001845/255); the sky strip is pixel-identical
between before/after in both scenes.

At 1280x720, 4x MSAA, the logged average GPU frame times were:

| Scene | Before | 24 steps | 48 steps |
| --- | ---: | ---: | ---: |
| Hyjal day | 8.04 ms | 12.50 ms | — |
| Citadel evening | 1.81 ms | 2.87 ms | 3.68 ms |

These are indicative single-run measurements, not a performance benchmark:
the streamed daytime scene has minor particle/batching variation (1763 vs
1779 M2 draw calls). The static evening geometry is the stronger controlled
comparison. The prototype makes daylight distant terrain darker/bluer and
changes evening silhouettes only subtly; this is not proof of improved
realism. The per-fragment implementation is too expensive for the small
evening difference and should remain an experiment.

Full-resolution before/after PNGs, comparison sheets and
`comparison-metrics.json` are retained in `build-dev/scratch/aerial-march`.
The normal default runtime shaders were restored after the 48-step test.

## Dalaran landing platform toward Gundrak

`dalaran_gundrak.json` adds a full streamed-world pair: terrain and distant
terrain, buildings and trees, View Distance 10000 yards, Fog Thickness 0.8,
12:00, 1280x720 and 4x MSAA. This is not an isolated WMO fixture.

The position comes from local `TaxiNodes.dbc`, node 310 (Dalaran):
(5813.8901, 449.1300, 658.75). Per the user's request the camera is lifted
twelve yards above the platform datum to clear the parapet, then shifted
four yards camera-right as requested, plus a twenty-five-yard forward shift
to approach the parapet and clear the column. The final eye is
(5815.7700, 423.8819, 670.75). It looks toward node 331 (Gundrak):
(6897.6499, -4118.2300, 467.35), a heading of about -76.65 degrees in render
axes. This identifies the direction; it does not guarantee the temple itself
is unoccluded from the platform. The observer's lighting is sampled in Dalaran.

```powershell
./tools/scene_render/aerial_march_preview.ps1 -Scene dalaran_gundrak -Variant before
./tools/scene_render/aerial_march_preview.ps1 -Scene dalaran_gundrak -Variant after
```

The comparison uses the same reframed camera for both variants. Both captures
completed without ERROR entries or incomplete-terrain messages. Logged average
GPU times were 12.18 ms before and 18.42 ms with 24 steps; these remain
single-run measurements. The column stays along the left edge, with the
central landscape open and the parapet along the bottom.


## Three-way density preview

The requested lineup, left to right, keeps the same Dalaran camera and world:

1. `before`: original non-marched haze, Fog Thickness 0.8.
2. `thick`: 24-step ray marching, Fog Thickness 1.25 throughout the profile.
3. `far`: 24-step ray marching, Fog Thickness 0.8 with additional far-air density.

The far-only switch is disabled by default. It preserves extinction through
the authored profile's near half, then smoothly adds optical depth. The
additional depth reaches a multiplier of 1.25 / 0.8 after another halving of
transmission in the far portion. Surface extinction and ray-march integration
use the same modified profile; the geometry boundary fade remains separate.
This is an artistic density experiment, not a calibrated physical medium.

```powershell
./tools/scene_render/aerial_march_preview.ps1 -Scene dalaran_gundrak -Variant thick
./tools/scene_render/aerial_march_preview.ps1 -Scene dalaran_gundrak -Variant far
python tools/scene_render/compare_aerial_march.py
```

The script generates the 1.25 scene in the scratch directory without changing
the original scene. The comparison script retains all rendered pixels and
adds labels above a single row in `dalaran_gundrak-three-way.png`.

Both new captures completed without ERROR or incomplete-terrain log entries;
all seven variant fragment shaders passed glslc and spirv-val. Against the
retained ray-marched 0.8 capture, fixed column/parapet rectangles differ by
at most 1/255 per channel in the far-only variant. The retained three-way
metrics record the rectangles and the single-run GPU times (19.05 ms for
1.25 and 21.04 ms for far-only). These are still expensive reference previews.


## Kalimdor / Hyjal three-way preview

The same three variants are available for `hyjal_day`: the retained original
mode at 0.8, ray marching at 1.25, and ray marching with only far-air density
boosted at base 0.8. The camera stays at the existing Barrens viewpoint facing
Hyjal, at noon, with the full streamed landscape, buildings and trees.

```powershell
./tools/scene_render/aerial_march_preview.ps1 -Scene hyjal_day -Variant thick
./tools/scene_render/aerial_march_preview.ps1 -Scene hyjal_day -Variant far
python tools/scene_render/compare_aerial_march.py
```

The single-row output is `build-dev/scratch/aerial-march/hyjal_day-three-way.png`.
Three-way metrics now use scene-specific filenames so subsequent comparisons
do not overwrite each other's reports. Dalaran's fixed near-geometry sample
rectangles are not applied to other viewpoints.

The two new Hyjal captures completed without ERROR or incomplete-terrain
messages; all seven shader variants passed glslc and spirv-val. Single-run
GPU averages were 12.18 ms for 1.25 and 13.38 ms for far-only. The original
mode's retained 0.8 capture averaged 8.04 ms. These are indicative timings,
not a controlled performance benchmark.
