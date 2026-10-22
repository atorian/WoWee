# Rendering pipeline implementation

Branch: `codex/gpu-scene-pipeline`, based on `46b55353`.
Checkout: `D:\games\WoWee\build-dev\scratch\gpu-scene-pipeline\checkout`.
Requirements: [rendering-pipeline-requirements.md](../docs/rendering-pipeline-requirements.md).

This is ongoing implementation, not a completed deferred renderer. The primary
checkout remains on master with its concurrent uncommitted work preserved.

## Foundation

- Ported the previously tested incremental M2/WMO RT registration, terrain depth
  cache and RangeAllocator synchronization from the primary checkout. Their
  saved Dalaran reports remain the historical reference, not measurements of
  this branch.
- RT region extraction uses a spatial broad phase, a dense published membership
  list and dirty instances. A stationary camera updates only mutated placements;
  moving the camera checks nearby candidates and previous members. Mesh reference
  counts determine publication/removal without scanning every resident mesh.
- RT placements have source generations: remove/recreate at the same ID, mesh
  and transform still publishes replacement in the acknowledged delta journal.
- PagedFrameBuffer publishes immutable page versions. Every GPU slot tracks its
  own contents; delayed slots catch up across skipped snapshots, scene replacement
  invalidates old versions, and old CPU snapshots remain recordable.
- M2 bones use stable ranges and immutable pages. Pose computation advances a
  revision; extraction copies changed poses and GPU slots upload changed pages.
  This does not yet introduce compute skinning or unify shadow animation.
- SceneView makes camera matrices/position explicit. WMO reflection extraction
  and recording use the same reflected view. Group frustum culling is independent
  of the opt-in portal/tighter-distance diagnostics.

- WMO extracts primary, reflected and shadow lists in one placement/group
  traversal. Evaluated group records are shared by index; shadow-only groups
  do not select local lights. Portal visibility remains specific to each view.

## Frame graph

- Resources are single-assignment logical versions. A later writer waits for
  every reader of its predecessor, including callbacks registered out of order.
- Physical images/buffers are imported for each acquired frame slot. Declared
  stage/access/layout uses produce synchronization2 barriers; independent
  shader stages receive visibility from the writer. Reset clears physical imports.
- Cycles, ambiguous writers, undefined reads and undeclared accesses reject the
  graph with an error; recording never guesses a fallback order.
- RT prepare/trace/temporal/three filters now use physical graph imports and
  declared accesses. Shared scratch targets include readers from the preceding
  submission; no CPU wait was added.
- Six M2/WMO PNGs are byte-identical before and after RT graph conversion.
  Forced Khronos synchronization validation passed both GPU fixtures.
- Existing other pre-passes retain their local barriers until migrated to imports.
  This is a dependency foundation, not yet a G-buffer/RT pass conversion.

## Material boundary

- WMO geometry/texture evaluation now hands an explicit WmoSurface and
  WmoLightingMaterial to shared lighting code. POM, alpha test and normal
  sampling remain in geometry; nonlinear glow/transitions remain in lighting.
- The forward shader uses that same evaluator today. Three WMO regression PNGs
  remain byte-identical to the pre-refactor references with sync validation.
- [Surface layout and memory contract](../docs/rendering-pipeline-layout.md)
  selects three MRTs, per-sample MSAA handling and explicit transient lifetimes.
  The attachments and current-frame lighting are not yet connected to Renderer.

- WMO frame settings are pushed from the immutable snapshot; materials contain
  only resource properties. Removed the settings-change device idle and shared
  material rewrites, including the stale settings on previously hidden groups.
- WMO/character POM use one parameterized shader function.
- The MSAA water continuation imports the preceding depth resolve/layout writes
  with the same attachment dependency as the ordinary scene continuation.

## Validation so far

- Native dev/no-LTO full wowee client, renderer, scene_render and GPU mutation
  fixture builds passed.
- Eight CTest targets passed: range_allocator, rt_instance_changes,
  scene_spatial_index, paged_frame_buffer, rt_scene_publication, terrain_depth, wind_field, render_graph.
- A real WMO fixture initially failed `reflection cull used the primary camera`:
  the frustum check was gated by the disabled diagnostic culling flag. Corrected
  to the existing independent frustum setting; the fixture now passes.
- WMO bounds include the analytical maximum wind displacement of cloth, tested
  against the shader expression over phases, normals and cloth spans.
- GPU snapshot tests pass for M2 and WMO: source mutation cannot change an
  extracted frame; the next extraction changes pixels. Forced Khronos validation
  reports no renderer VUIDs (two pre-existing missing Epic overlay manifests).
- render_dependency_check: zero forbidden includes. record_scene_check: zero
  live scene reads in recording. sweep_guard: every sweep within its ceiling.
- The two graph regressions fail on the original implementation and pass after
  correction. Native GPU fill/copy/readback verifies readers retain both old and
  replaced versions; forced validation reports no renderer errors.
- The WMO settings fixture failed on the original implementation when a hidden
  WMO appeared after a settings change. It passes after the fix, including
  settings changed after extraction, MSAA 4x and the water reflection path.
  Synchronization validation exposed and verified the MSAA depth LOAD fix.
- Single-traversal WMO preserves all six M2/WMO RT mutation PNGs byte for byte.
  Opposite primary/reflected cameras and hidden placements preserve independent
  pass membership and already queued snapshots. Water/reflection settings
  regression also passes with synchronization validation.
- The 2026-10-22 Dalaran replay completed on this branch. No overall FPS
  improvement was measured; detailed results follow.

## Dalaran measurement — 2026-10-22

[Full machine-readable report](performance/dalaran-pipeline-foundation.json).
Implementation commit `5746c8d`; saved comparison is
`performance/dalaran-rt-instance-changes-after.json`. Both use a measured
2560x1369 viewport, render thread 1, hardware RT mode 3, MSAA 1 and dev/no-LTO.
Validation is disabled. All 760 source samples completed (0–759), with 759
producer frame intervals; trace error is empty and the client exited normally.

| Metric | Saved baseline | Foundation branch |
|---|---:|---:|
| FPS (1000 / mean frame interval) | 42.46 | 41.96 |
| Mean frame interval, ms | 23.551 | 23.833 |
| p95 frame interval, ms | 33.404 | 32.901 |
| p99 frame interval, ms | 44.012 | 45.003 |
| CPU scene extraction, mean ms | 12.242 | 11.320 |
| CPU WMO extraction, mean ms | 0.640 | 0.383 |
| CPU RT region extraction, mean ms | 1.928 | 1.316 |
| CPU M2 extraction, mean ms | 6.424 | 6.245 |
| GPU WMO, mean ms | 5.039 | 4.617 |
| GPU RT, mean ms | 3.101 | 3.289 |
| Reported VRAM usage, mean GiB | 4.274 | 4.425 |
| Reported VRAM usage, max GiB | 4.425 | 4.425 |

Overall FPS is 1.2% lower; one run does not establish a regression or a
statistically significant improvement. This is a historical comparison, not
an isolated A/B. Resident populations differ because loading is asynchronous:
mean characters 446.6 → 440.7, M2 104456.8 → 104413.6, WMO 792.7 → 792.6.
All 3040 comparisons of character/M2/tile/WMO populations against the original
capture differ, as in the saved baseline. Entity populations match the baseline.
Nested CPU scopes and concurrent recording must not be summed.

Sampled process memory maxima (500ms polling, loading + replay): working set
5.093 GiB, private commit 9.677 GiB. There is no previous process-memory baseline;
private commit is not resident RAM. VRAM numbers use the renderer's heap-usage
counter. Other wowee processes were absent at preflight; there was no continuous
competing-process monitor.

One ERROR occurred during warmup, before measured frames: `Unprepared character
texture reached GPU consumer: character\tauren\taurenmalefacelower04_18.blp`.
This benchmark does not establish its cause or fix it. Do not call this an
error-free visual-parity run.

Raw trace, CSV and comparison: `build-dev/scratch/scene-replay/run-20261022-110258-583/`.
Process memory and runner logs: `../replay-memory.json`, `../replay-output.log`,
`../replay-errors.log`, relative to this checkout. The replay client has exited.

## Remaining implementation

- [x] Finish foundation GPU snapshot/lifetime tests and guards.
- [ ] Record exact material/depth/blend contracts and choose G-buffer packing,
      MSAA resolve and device capability/memory budgets.
- [ ] Implement opaque/cutout G-buffer and material lighting with RT disabled;
      preserve forward ordering for incompatible blended materials.
- [ ] Route current-frame receivers into RT, centralized denoise/resolve and
      lighting; explicit motion/history and missing half-resolution samples.
- [ ] Complete transparent/water/reflection lighting and atmospheric composition;
      remove previous-frame recovery from converted consumers.
- [ ] Persistent GPU instance/material tables and GPU culling/indirect commands;
      one shared pose preparation, measure optional skin cache.
- [ ] Full scene_render correctness matrix, threaded replay, memory/CPU/GPU
      report versus saved Dalaran baseline and final cleanup.

Scratch scripts/logs are in `D:\games\WoWee\build-dev\scratch\gpu-scene-pipeline`:
`build.ps1`, `build.log`, `check-gpu.ps1`, `check-gpu.log`, `renderer-changes.log`.
All compiler temp/cache and runtime data stay on D:. Windows tools require the
approved WSL interop execution path in this session; ordinary sandbox invocation
fails with `UtilBindVsockAnyPort`. One approval review timed out; its allowed
single retry succeeded. No sandbox or global configuration was changed.
