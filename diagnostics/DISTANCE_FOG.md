# Distance fog — 2026-10-11

The follow-up yellow silhouettes and missing island are addressed in [HORIZON_BACKGROUND.md](HORIZON_BACKGROUND.md). The results and binary hash below describe the first iteration.

The supplied screenshot shows far Northrend buildings as bright silhouettes against the purple Dalaran sky. The user reports the same problem on ships and zeppelins. The current game process was `build/bin/wowee-terrain-fix.exe`; saved fog strength is 0.5 and sky tint is 0.7.

## Causes in the rendering code

`LightingManager::update` started its weighted outdoor sum with `LightingParams{}`. That expression retains the struct's nonzero member initializers. Every outdoor blend consequently added fallback fog (0.5, 0.6, 0.7), fallback mid-sky (0.7, 0.85, 1.0), and 1000 yards of fog end, along with extra ambient/direct light and the remaining sky bands. The subsequent fog/sky mix could exceed display range and appear white. Its distance was also incorrectly extended before the player's strength multiplier.

The CPU then mixed fog toward a single middle-sky color, regardless of the direction of the object. Objects below the horizon received the same bright tint as those in front of higher sky. Beyond the fog end the tint never changed, preserving the same flat silhouette at any further distance.

WMO window reflections and highlights were computed after fog, allowing glass to regain brightness with distance.

## Changes

- Start weighted sums with an explicitly zero accumulator; preserve normal fallback defaults.
- Keep authored fog separate from sky colors. Append four directional sky colors to the frame UBO without changing its existing field offsets.
- Share the actual skybox gradient/scattering math with terrain, WMO buildings/transports, M2 models, characters, grass and water. Tint haze along the camera-to-surface ray, including the below-horizon band.
- Smoothly increase background matching beyond the authored fog end. A zero sky-tint setting retains authored fog, and zero fog strength retains clear surfaces.
- Apply WMO fog after glass reflections. Disable sky tint for indoor lighting, underwater fog and character previews; reflections use their reflected camera position.

## Validation

`test_light_bands` runs 207 assertions in 15 cases, including `test_distance_fog.cpp`. The regression includes the actual `sky_atmosphere.glsli` math through GLM rather than reimplementing the formulas. It covers zero-initialized blending, horizon/nadir tint, far-distance convergence, user-disabled tint, nearby visibility and degenerate ranges.

All seven affected fragment shaders compile and pass `spirv-val`. The shader-function checker reports no divergent duplicate functions. Full client and startup validation are recorded in `build/distance-fog-build.log` and the isolated `distance-fog-smoke` files.

These checks verify the code, shader math and startup. They do not constitute a same-scene visual comparison of Gundrak or moving transports; that remains to be checked in game after restarting with the updated executable.

The full Windows Release build completed successfully. The isolated startup produced `build/distance-fog-smoke.png` and logged a clean application exit with no errors. All seven runtime shader SHA-256 hashes match source SPIR-V. The tested executable is installed as `build/bin/wowee.exe` and retained as `build/bin/wowee-distance-fog.exe` (19,243,836 bytes, SHA-256 `6DE48DA47049824E33E7E314F25CA63FC9C0837222F1908C6275803C97F13FD5`). Restart through `wowee.exe`; the already-running `wowee-terrain-fix.exe` still contains the previous frame-buffer layout.
