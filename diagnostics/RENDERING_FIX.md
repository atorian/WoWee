# Dalaran rendering diagnosis — 2026-10-11

The user's stock-client reference shows visible warm torch and blue portal lighting in the same interior that appears almost black in WoWee.

## Evidence

- The active client is `build/bin/wowee-fast-login.exe`, which logs to `build/bin/logs/wowee-fast-login.log`. The earlier `wowee.log` belongs to another session.
- The renderer selects AMD Radeon RX 7700 XT. At 14:24:13, the active log reports 9.31 FPS, 107.44 ms/frame, 71.30 ms in `gameHandler->update`, and 13.25 ms of GPU passes. This is substantial CPU overhead.
- A bounded 200-sample read of the main thread's instruction pointer, symbolized against that exact executable, repeatedly lands in `WMORenderer::rebuildSpatialIndex` (RVAs `0x540037`, `0x540414`, `0x540418`, `0x540433`, and adjacent instructions). The diagnostic resumes the thread in a `finally` block after each sample.
- `TransportManager::update` pushes every active hull's transform per frame. `WMORenderer::setInstanceTransform` previously rebuilt the grid and ID lookup for every building on every push.
- Dalaran's root MOHD contains ambient BGRA `(90,66,53,255)`. Its indoor MOCV channels contain low-valued warm/cool additions. The old shader used a component-wise maximum with ambient, then multiplied by an outdoor shadow clamped to 0.45. This suppresses authored color variation and darkens baked lighting again.

## Changes

- Refile the moved WMO's old/new bounds using the existing spatial-grid helper; keep all other objects and the ID lookup intact.
- Interior WMO shading adds base ambient and twice the authored vertex color, with the existing 0.15 safety floor. Missing vertex colors use neutral modulation. Indoor baked light is no longer multiplied by the outdoor shadow map.
- The MapObj 2× modulation is recorded in the original shader analysis: [Wowser WIP Wrath Shading](https://github.com/wowserhq/wowser/wiki/WIP-Wrath-Shading). Full stock-client vertex preprocessing and all material permutations remain outside this change; the screenshots must still be compared after restarting the client.

## Validation

- Windows Release client build completed successfully. GLSL compiled successfully; the tracked and runtime SPIR-V files have identical SHA-256 hashes.
- Existing spatial-grid tests passed: 22 test cases, 64 assertions, including movement across cell boundaries and unchanged membership inside a cell.
- An isolated-config startup run selected AMD Radeon RX 7700 XT, rendered the login screen to `build/render-smoke.png`, and logged a successful shutdown. No error-level entries or stderr output were recorded. This startup check does not measure in-world FPS or verify the Dalaran lighting.
- A synthetic 239-building Linux benchmark measured a full rebuild at 0.4248 ms versus a moved-bounds update at 0.0003 ms, including cell crossings. This demonstrates the algorithmic difference; it is not an in-game FPS measurement.
- The current running process retains the old executable and pipelines. Use the rebuilt `build/bin/wowee.exe` for the next in-world FPS and visual comparison.

## User verification

The user confirmed that rendering is better after the change, while still not matching the original perfectly. Visual parity and the new in-world FPS remain unquantified.
