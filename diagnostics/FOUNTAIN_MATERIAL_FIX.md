# Dalaran fountain materials — 2026-10-11

The user's WoWee screenshot shows bright waterfall sheets, a bright foam ring and white crystal faces compared with the stock fountain. The extracted `dalaran_fountain_01` model also contains water/crystal geometry absent from the stock comparison; fixing material semantics does not establish identical geometry.

Verified assets: `Data/expansions/wotlk/world/expansion02/doodads/dalaran/dalaran_fountain_01.m2` (25 batches) and `dalaran_fountain_02.m2` (4 batches), each with its `00.skin`. The original BLP textures were not edited. Pillow confirmed alpha ranges 0–189 on DALARAN_FOUNTAINWATER_01 and 0–158 on BE_FOUNTAIN_WATER03; BE_FOUNTAIN_WATER05 has constant alpha 170/255.

Renderer corrections:

- Both opaque instancing and transparent draws carry the batch's existing opacity into the instance SSBO. Previously this value only decided whether to skip a batch; all remaining layers drew at full instance opacity. Transparency combos now resolve through the lookup table. Existing initial-zero fallback is retained for animated at-rest tracks.
- Shader-0, two-stage alpha-add materials combine RGB and alpha with the second texture outside the sky path too. Both fountain models' foam pairs WATERFOAMLOOP2 on scrolling UV0 with T_VFX_BORDER3 on static UV1. Statue shader-1 reflection combiners remain separate. The second texture is protected from eviction while a loaded model references it.
- Regular transparent doodads honor the authored single-sided flag in the fragment shader. Crystal batch 8 has flags 0x11 (single-sided); water batch 4 has 0x15 (two-sided). The unused material fade slot was reused for culling, preserving the UBO layout. Sky and spell paths retain their existing face handling.

Independent semantics reference: [noclip's M2 material and texture-combo implementation](https://github.com/magcius/noclip.website/blob/main/src/WorldOfWarcraft/data.ts), [RGBA combiners](https://github.com/magcius/noclip.website/blob/main/src/WorldOfWarcraft/program.ts).

Validation completed: 18 test cases / 331 assertions across material, blend-mode and sky regressions, including both real fountain assets. M2 fragment SPIR-V passed `spirv-val --target-env vulkan1.0` with the required child-process runtime PATH.

The offline `WOWEE_FOUNTAIN_PROBE` fixture loads the real M2 and records a fixed camera/animation through the world renderer without realm authentication. `WOWEE_FOUNTAIN_PROBE_LEGACY` reproduces the prior missing mask/weight stages and drawing of back faces within this fixture. The probe hides the login UI so the scene is visible.

Concurrent builds in the shared directory caused a missing-object LTO read and Ninja recompaction `Permission denied`. `link-fountain-snapshot.py` links 465 stable COFF copies with a separate response file and binary; its object manifest records SHA256 hashes.

The initial AA=0 world probe exposed an existing scene-continuation render-pass compatibility VUID, before the fountain fix. AA=2 passed that initial validation. Do not describe AA=0 as validation-clean or treat a stock-scene comparison as verified by this offline fixture.
