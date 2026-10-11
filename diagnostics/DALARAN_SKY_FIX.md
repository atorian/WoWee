# Dalaran sky — 2026-10-11

Stock comparison shows pink clouds and subtle aurora strands, rather than broad bright yellow cards. Preserve the authored effect.

Verified assets: `Data/expansions/wotlk/environments/stars/dalaranskybox.m2` and `dalaranskybox00.skin`.

- 99 batches; 21 mode-4 additive, shader-0 batches use two textures. Stage 0 is AURORALIGHT_OR_B on UV0, stage 1 is AURORAMASK_B_ORANGE on UV1. The mask has grayscale RGB (0..144) and alpha (0..123); multiply both stages. Previously only the light texture was bound.
- UV combo table is `[0, 1]`; resolve the combo index.
- Sky Stand duration is 320000 ms. Map visual game time over that duration. Day/sunset/night clouds have weight tracks 2/3/4. At 15:00 day=1, sunset/night/stars=0. Previously initial keys were baked and initial zero keys ignored.
- Sample color alpha and resolved texture weights per draw; multiply instance fade to retain zone crossfades. Cloud global fades and UV tracks keep their independent elapsed clocks.
- Supply a valid second sampler fallback to every material set; sample it only for supported sky masks. Expand the descriptor pool sampler budget.

Validation: `test_m2_sky_material` passed 3 cases / 148 assertions, including actual extracted Dalaran model and all 21 mask batches. This verifies assets and authored alpha values; world visual parity still requires an in-game comparison.

Keep second-stage sky masks in the cache in-use set alongside primary textures, so cache eviction cannot invalidate a live material descriptor.

Final verification:

- Final Release client and all required shaders built successfully. Concurrent client/probe response-file conflict was avoided with `CMakeFiles/wowee-sky.rsp`.
- `m2.vert.spv` and `m2.frag.spv` passed `spirv-val --target-env vulkan1.0`.
- Camera, blend/opaque-alpha and sky regressions: 25 cases / 236 assertions passed.
- Installed `build/bin/wowee.exe`: 19242171 bytes, SHA256 `261253fff5e704ec37a7f96a6ff5d684b42883cb21cb80eb453d2fdecb8864fb`.
- Verified matching source/runtime copies of 10 surface/sky/RT shader binaries.
- Installed client's isolated login-screen smoke produced `build/sky-smoke.png`, exited cleanly, and logged `Application exited successfully` at 15:52:24. No ERROR entries. User game settings were not used for this smoke.
- This binary also includes the concurrent RT camera-relative reconstruction and geometry-normal shadow-bias fixes; it preserves the camera/mount corrections.

Same-scene stock parity remains a manual world check, particularly sky appearance and remaining mountain artifacts. The login-screen smoke is startup coverage only.
