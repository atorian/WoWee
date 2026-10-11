# Dalaran camera, mount and terrain report — 2026-10-11

The attached screenshot shows terrain contour bands and holes in the player's mount. The user also reports that zoom out stops after one step.

## Confirmed code defects

- `CameraController` applied the six-yard interior cap to any containing WMO group, including outdoor city platforms. It now checks `cachedInsideInteriorWMO` for both wheel input and the automatic indoor cap.
- Collision feedback compared requested zoom with the smoothed collision distance even when no obstruction was present. Zoom smoothing alone could therefore shrink the requested distance. Outdoor collision now leaves the requested distance intact; indoor feedback additionally requires an obstruction.
- `CharacterRenderer` inferred alpha testing for opaque creature/mount materials from texture transparency. Opaque atlas alpha may be unused. Opaque batches now skip this inference and explicitly ignore texture alpha, while instance/animation fades and hair cutouts remain supported.
- Surface shaders used normalized shadow-map UV texel size as a world-space normal offset. At a 500-yard half extent this was 1000 times too small. `shadowParams.w` now supplies world texel size, shared by terrain, characters, WMOs and M2 doodads.
- RT geometry distance used float16, whose spacing reaches two world units at distance 2048. The geometry buffer and its storage declaration now use float32; radiance/history color buffers remain float16.

## Validation and limits

- Camera/pivot regressions: 13 test cases, 45 assertions passed.
- M2/material regressions: 9 test cases, 43 assertions passed.
- Updated GLSL shaders compiled successfully, including the single-sample and MSAA RT preparation variants.
- Windows Release `build/bin/wowee.exe` was rebuilt and replaced. Updated runtime SPIR-V hashes match the tracked files.
- Isolated startup screenshot check passed; the client logged a clean shutdown.
- Hardware RT probe passed on AMD Radeon RX 7700 XT: the occluded wall sample was 0.018 sun visibility, both unoccluded samples were 1.000, and all output values were finite.
- Compute RT fallback probe also passed with the same reference samples and finite outputs (exit 0).
- A same-scene in-game comparison is still needed to establish which terrain artifacts these changes eliminate. The screenshot alone does not establish the cause of every sky seam or model artifact.
