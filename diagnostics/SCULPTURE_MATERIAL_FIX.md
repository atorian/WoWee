# Dalaran sculpture materials — 2026-10-11

User comparison: WoWee shows white reflection streaks and the inner Kirin Tor symbol on the top orb. The stock screenshot shows purple crystal surfaces and a golden orb.

Verified extracted asset: `Data/expansions/wotlk/world/expansion02/doodads/dalaran/dalaran_sculpture.m2`, with its `00.skin` file. All 14 batches and their textures are present. No extracted texture was modified.

Confirmed renderer defects:

- Batches 2/5/8/11 select texture-coordinate combo 1, which resolves to **-1** (eye-space environment mapping). They use ARMORREFLECT4 / ORBREFLECTBRIGHT. Previously -1 became UV0, projecting the reflection texture as a stripe atlas. Preserve the signed selector and generate eye-space coordinates from the skinned/transformed position and normal in the real M2 vertex shader.
- Those four batches use **Mod2x (6)**. Previously every blend mode above 2 used the additive Vulkan pipeline. Add Mod (5) and Mod2x (6) pipelines with destination-color multiplication; alpha is ignored and destination alpha is preserved. Neither mode receives atlas-alpha sharpening or a black color key. Fog and instance fade approach the multiplication identity (1 for Mod, 0.5 for Mod2x), preserving the surface behind them.
- Batch 12 is the inner orb symbol and has **priorityPlane -1**; the gold shell and its reflection have priority 0. The renderer ignored the signed priority and drew the symbol last. Build a stable priority order once per uploaded model and use it for the transparent pass; retain source order within each priority and keep the batch array/descriptors/sky diagnostic indices intact.

References for independent material semantics: [M2 format specification](https://github.com/sc2-arcade-watcher/WhiteoutLib/blob/libmodkit/docs/M2_FILE_FORMAT_SPECIFICATION.md), [noclip eye-space mapping](https://github.com/magcius/noclip.website/blob/main/src/WorldOfWarcraft/program.ts), [noclip authored priority ordering](https://github.com/magcius/noclip.website/blob/main/src/WorldOfWarcraft/data.ts).

Final validation:

- Material, blend and sky tests: **16 test cases / 263 assertions passed**, including all four reflection layers and the inner orb's signed priority in the real extracted model.
- Final Release build completed successfully. Client output and linker response file were isolated as `wowee-sculpture-fix.exe` / `wowee-sculpture.rsp`.
- Both M2 SPIR-V files passed `spirv-val --target-env vulkan1.0` with the MSYS2 runtime on the child process PATH. All eight checked M2/surface/sky shader binaries match their runtime copies.
- Isolated startup smoke produced `build/sculpture-fix-smoke.png`, exited cleanly, and logged `Application exited successfully` at 17:11:58 with no ERROR entries. Smoke used a separate configuration root and log.
- Installed `build/bin/wowee.exe`: **19,256,536 bytes**, SHA256 `9B39CABA98E7EB98EB5570353C14D5E2748D1F31C3BB04E351CFF4043CE6D3A4`.

The active `wowee-terrain-fix.exe` process was left running. The user must start `build/bin/wowee.exe` to use this correction. Startup and material-data tests are distinct from a same-scene stock comparison; visual parity is not yet verified in the running world.
