# Dalaran glass and lamp shadows — 2026-10-11

The reported exterior windows were shaded with a blue Fresnel tint and alpha 0.4–0.95. Their apparent landscape was transmission of the background through the pane, not reflection of the surroundings. Exterior panes now output alpha 1. Explicit instance/dungeon glass keeps its existing transparency.

With ray traced lighting enabled, the pass also traces one reflected ray into the local scene, using the same hardware or compute acceleration structure as lighting. Buildings behind the camera can therefore appear in glass. Results are held in a separate per-frame reflection image, without diffuse temporal accumulation or wide spatial filtering. The lookup checks the receiving plane's distance and normal and uses the atmospheric sky when no matching reflection exists. The approximation uses the scene's averaged material colors, within 120 yards, at half resolution; it does not include characters or individual texture details. RT off retains an opaque, sky-tinted pane.

The real `Dalaran_Lamppost01` has three opaque layers and four blended layers, including environment highlights and a light volume. The shadow-map pass previously drew all seven, without alpha testing solid non-foliage cutouts. It now shares its opaque/cutout participation predicate with RT registration, skips blended/glow/hidden layers, and binds each cutout's texture independently of foliage wind. Ordinary opaque metal still casts a shadow.

Forward RT lighting lookup now verifies geometric normals as well as receiver distance. This rejects near-coincident lid, side and back surfaces before their shadow/AO can be transferred to another side of a thin model during camera movement.

## Validation

- Real lamp/material tests: 170 assertions across seven cases, including both Dalaran lamp models.
- All four forward shaders, compute and hardware trace variants compile.
- `rt_probe` tests a reflected red building behind the camera, ordinary sun shadows, zero self-shadow/AO speckles, and twelve production surface-lookup cases, including perpendicular and opposite-facing receivers.
- Hardware and compute paths pass at origin and (5800, 600, 650), with Khronos Vulkan validation enabled. Deferred resources are drained while their owners/pools remain alive before the fixture shuts down.
- With RT Full and MSAA, the final client passes the actual Dalaran scene with no renderer/Vulkan validation errors and zero surviving VMA allocations. An offline client fixture loads all 91 actual Dalaran WMO groups and the real seven-layer lamp. It uses separate settings, assets and runtime paths and records the real world rendering pass, without logging into a realm.

Reproduction scripts: `build-glass-lamps.ps1`, `compile-glass-lamps.ps1`, `link-glass-lamps.ps1`, `run-glass-lamps-probe.ps1`, `glass-lamps-scene.ps1`. Client objects and link snapshots are isolated from concurrent Ninja builds.

RT off and single-sample rendering also pass the real-city fixture. The temporary runtime is `build/glass-lamps-runtime`; `build/start-glass-lamps.bat` starts its verified client with the project's extracted Data and ordinary user settings. Primary replacement uses `install-glass-lamps.ps1`, checks that the normal client is closed, backs up the executable and shaders, and verifies hashes.
