# Horizon background — 2026-10-11

The follow-up screenshot still has yellow Gundrak/transport silhouettes against the purple Dalaran sky, and a distant castle with no visible island underneath it. This supersedes the first directional-gradient treatment in DISTANCE_FOG.md.

## Causes

The procedural DBC sky gradient is not always the final sky. Authored M2 layers cover it in Dalaran and other zones; matching a yellow analytical gradient preserves conspicuous yellow silhouettes against their purple sky. Separately, WMO portal/group culling is disabled by default to avoid disappearing interiors. That also bypassed its previous distance test, leaving cached buildings and transports drawn beyond the terrain's horizontal view limit.

## Implementation

A small current-frame sky image includes the procedural background, authored sky M2 layers and the current fog volume. Lens flares are excluded because they are optical overlays. Surface haze samples this actual background using the same view/projection as the scene; it is not given another volumetric-fog application. Analytic sky math remains the fallback for standalone previews and reflections.

The image is 256 pixels wide and follows the scene aspect ratio. Each of the two fenced frame slots has its own MSAA-compatible attachments. A separate capture descriptor set prevents sampling the image being drawn. All descriptors are updated before any frame pre-pass uses them, including the fog compute pass. The capture runs after volumetric fog and before the full-resolution scene pass, avoiding an additional full-resolution clear/resolve.

Haze converges on the background by the authored fog end with the saved 0.7 sky-tint setting. Detailed geometry fades through the final 15% of the view-distance setting, then clips at that common horizontal limit. This applies to terrain, WMO buildings and transports, M2 models, characters, grass and water. Multiplicative and additive M2 materials fade toward their respective neutral contributions. Authored sky M2s are exempt.

WMO groups use a conservative nearest-AABB distance independently of optional portal culling. Terrain includes chunks whose bounds intersect the view circle; testing only chunk centers used to remove terrain early at the outer edge.

This change does not add WDL/terrain LOD. Beyond the detailed terrain range, unsupported distant buildings disappear with the terrain. A later far-terrain implementation would allow the complete island/castle landscape to remain visible farther away.

## Comparison with WoW

Blizzard's [Engineer’s Workshop: Extended Draw Distance](https://worldofwarcraft.blizzard.com/en-us/news/20139979/engineer-s-workshop-extended-draw-distance) describes separate fully-fogged and geometry-clipping distances, plus reduced-detail distant terrain/water/models/buildings. The [Forever overview](https://worldofwarcraft.blizzard.com/en-us/forever/) confirms upgraded lighting and environments, but the reviewed official sources do not publish the exact atmospheric formulas. This patch is not claimed to reproduce Forever's renderer.

## Validation

- The production GLSL atmosphere helper is included directly in the C++ light/fog tests: 216 assertions, 16 cases, including a purple authored sky absorbing a yellow-tinted distant ship and the shared view fade.
- Seven surface/sky fragment shaders compile and pass SPIR-V validation. The duplicate shader-function checker has no divergent copies.
- `sky-background-probe.ps1` runs the original Dalaran sky offline, with separate configuration roots, frame-slot reuse, clean teardown and Vulkan validation for no MSAA, 4x and 8x MSAA. All three modes passed on Radeon RX 7700 XT: authored sky loaded/drawn, both frame slots reused, clean exit, and no Vulkan VUIDs. The test forces the Khronos layer from a local runtime in `build/validation-runtime`; it uses inline command recording and a short frame recording for termination.
- These checks do not substitute for a same-position, same-time in-game comparison of Gundrak, transports and the island castle.

The first strict run exposed existing diagnostics faults at login: GPU timestamps issued inside a secondary-only render pass, and screenshot readback using an already-presented swapchain image. These are outside the sky-capture pass. The final fixture uses `WOWEE_SINGLE_THREAD_RECORD=1` and `WOWEE_RECORD`, without ignoring these VUIDs. Only the two known missing Epic overlay JSON loader messages are excluded. The initial error log is retained as `build/sky-background-parallel-login-validation.log`. Parallel world rendering and a same-scene landmark comparison remain unverified.

The official MSYS2 validation-layer archive was downloaded into the build directory and verified against SHA-256 `5e6922e311c7c1702ddf09f95e55fe2e0682793d3dbd049c06746545d49a6870`. Only its DLL and JSON manifest were extracted; no system installation, registry or global environment settings were changed.

The final Windows Release client built successfully. Normal startup/screenshot/clean-shutdown smoke passed. Runtime surface/sky shader hashes match the source SPIR-V. Installed `build/bin/wowee.exe` and retained `build/bin/wowee-distance-fog.exe`: 19,263,989 bytes, SHA-256 `BB31489586DBB20FC02982C086D233ECC61C53DFC23ABC34DBF1B391CC84DCCA`.
