# RT movement halo — 2026-10-11

The user confirmed that the pale halo around the moving character disappears with ray-traced lighting off. FSR was already disabled (`upscaling_mode=0`, `fsr=0`); the saved RT mode was Full (3).

## Cause and change

The forward surface shader reads the previous frame's half-resolution RT lighting. Its local 3x3 depth-matched lookup can have no sample for a moved silhouette or the background just uncovered by it. It then substitutes flat ambient, which is brighter than the traced ambient in this scene.

`assets/shaders/rt_lighting.glsli` now predicts neighbouring receiver distances from the fragment's screen-space distance gradient. If the local lookup has insufficient confidence, it searches eight taps at radii 2, 4, 8 and 16 trace pixels, stopping at the first confident ring. Wider taps require a stricter depth match, and out-of-image taps are rejected. Stable surfaces retain the local lookup; when there is no matching surface anywhere in the bounded search, the normal fallback remains.

## Validation

The GPU fixture in `tools/rt_lookup_probe.comp.glsl` calls the actual production lookup. `tools/rt_lookup_probe.hpp`, invoked by `rt_probe`, supplies a previous-frame foreground silhouette and a sloping background with distinct lighting. It checks the shifted character edge, uncovered background, a wider uncovered strip, stable lighting, depth rejection, RT off, AO-only, sun-only, and image boundaries.

With the previous local-only lookup, five checks fail: the moving edge, both uncovered-background samples and the moving edge in AO-only and sun-only modes. The full-RT failed samples substitute ambient (0.30, 0.35, 0.40). With the change, they recover the expected foreground (0.02, 0.03, 0.04) or background (0.08, 0.10, 0.12).

All nine checks pass on AMD Radeon RX 7700 XT, alongside the existing rotated receiving-wall trace regression, for hardware ray queries and compute tracing, both at the origin and at camera coordinates (5800, 600, 650). The unoccluded wall retains zero false AO/shadow texels.

Output: `build/rt-halo-before.out` and `build/rt-halo-{hardware,compute}-{origin,translated}.out`. The isolated probe is `build/bin/rt-halo-probe.exe`. The scripts in `diagnostics/compile-rt-halo.ps1` and `diagnostics/run-rt-halo-probe.ps1` reproduce the local comparison; a normal build with `WOWEE_BUILD_RT_PROBE=ON` compiles the fixture through CMake.

The character, M2, terrain and WMO fragment SPIR-V files are compiled and installed in both source assets and `build/bin/assets/shaders`, with matching SHA-256 hashes. Restart WoWee to create pipelines with them, then compare the same in-game movement with RT enabled.

## Limits

The GPU test verifies lookup recovery on simulated movement/disocclusion and compilation of all four consuming fragment shaders. The reported in-game scene has not yet been compared after this change. The bounded spatial recovery approximates nearby lighting; it cannot provide a new trace where an entirely new surface has no matching neighbour.
