# Render speckles — 2026-10-11

The user's screenshot shows dark stippling on Dalaran walls and the nearby surface. The active process was `build/bin/wowee.exe`, with full ray-traced lighting, normal mapping and POM enabled.

## Reproduced cause

The RT pass inverted a float32 absolute world-view-projection matrix and reconstructed normals by subtracting nearby absolute world positions. At large map coordinates those small pixel differences lose precision. Incorrect normals send ambient-occlusion rays back into the receiving wall.

The original probe had no receiving wall in the acceleration structure and ran at 256×256 near the origin. It passed even when the reported defect remained.

The extended `tools/rt_probe.cpp` uses a rotated wall, a real wall mesh, 1024×1024 depth, and checks a region that has no legitimate occluders. `--translated` moves the scene and camera to (5800, 600, 650).

| Backend | Location | False AO/shadow texels before | After |
| --- | --- | ---: | ---: |
| Hardware ray queries | Origin | 0 | 0 |
| Hardware ray queries | Translated | 50,527 | 0 |
| Compute fallback | Origin | 0 | 0 |
| Compute fallback | Translated | 50,521 | 0 |

Before the fix, minimum AO visibility in the unoccluded translated wall region was approximately 0.315. After the fix it is 0.999. Both backends retain the real occluder's shadow, and all lighting values remain finite. Detailed before/after output is in `build/rt-artifact-*.out`.

## Changes

- Supply view and projection separately to RT. Remove camera translation before multiplying and inverting them.
- Reconstruct depth, normal differences and ray directions in camera-relative world coordinates; add absolute camera position only when querying scene geometry.
- Keep temporal and surface reprojection consistent with camera-relative matrices.
- Base shadow-map receiver offsets and slope bias on unperturbed geometry normals, oriented toward the light, across WMO, terrain, character and M2 shaders.

## Built client

- Windows Release client: `build/bin/wowee-render-speckles.exe`.
- Isolated startup check passed: `build/render-speckles-smoke.png` was produced, no error entries were found, and the log confirmed `Application exited successfully`.
- Runtime SPIR-V matches the compiled WMO, terrain, character, M2, RT prepare/MSAA, trace/hardware, temporal and filter shader files.
- The ordinary `wowee.exe` output is also being built by another active chat. This fix is provided as a separate executable to avoid replacing that chat's output.

## Validation scope

The GPU regression verifies the reproduced numerical defect on AMD Radeon RX 7700 XT. It does not establish that every artifact in the user's scene has been eliminated. An in-game comparison after restarting the rebuilt client is still required.
