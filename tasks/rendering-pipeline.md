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
  The initial RT-disabled WMO connection is described below; current-frame RT remains pending.

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

## Follow-up fixes — 2026-10-22

- Character texture workers now publish failed decodes as completed results.
  An absent result remains a publication error, reported once per path. A later
  valid result supersedes the failed source immediately rather than waiting for
  512 unrelated texture lookups. Prepared-only uploads read the published pixels
  directly instead of cloning the decoded image on the producer thread.
- The GPU regression first failed on the previous implementation: a valid
  prepared image remained white after the prior failure. It now passes with
  synchronization validation, including a real upload.
- M2 frames retain one immutable resource publication shared across main and
  shadow draw lists. Draw records use its pinned model pointers, eliminating
  per-instance model-map lookups and shared-pointer reference operations on
  visibility workers and shadow list construction. Model load/removal/material
  version changes invalidate the producer publication; unchanged frames reuse it.
  Publications currently contain all loaded model versions and are rebuilt on
  those changes, not per frame. GPU retirement still uses the existing snapshot
  lease/fence contract.
- Draw-buffer capacity is accumulated during visibility classification; removed
  the second traversal of completed M2 draws. Separate CPU scopes expose pose
  publication, candidate gathering, classification, emitters and shadow lists.
- Full dev client, scene_render and native GPU fixture builds passed. M2/WMO
  source-removal tests preserve the queued image and change the next frame;
  synchronization validation reports no renderer errors. Dependency and recording
  guards still report zero forbidden includes/live scene reads.

A replay after the texture change (before the M2 resource change) aborted during
loading with `std::bad_alloc`; **zero measured frames**, so it provides no FPS
comparison. Before the failure the resource monitor reported system commit
43.91/45.18 GiB, only 1.27 GiB available, despite free physical RAM. Process maxima
sampled across this incomplete run were 4.263 GiB working set and 8.000 GiB
private commit. System pressure is evidenced; the allocation site/stack was not
captured, so the exact failing allocation is unknown. Do not call the OOM fixed.
The earlier successful replay also logged low system commit during loading.

Preserved failure evidence: `../replay-texture-oom.log` and
`../replay-texture-oom-memory.json`; successful follow-up validation:
`../verify-m2-resources.log`, `../renderer-changes.log`, `../gpu-m2/`, `../gpu-wmo/`.
The last completed FPS result remains **41.96 FPS**, predating these fixes.
Current-frame RT, G-buffer lighting and GPU indirect submission remain unfinished.

## Successful follow-up replay — 2026-10-22

[Full report](performance/dalaran-m2-resource-publication.json), implementation
`d440fce`. All 760 samples completed; client exit was normal, trace error empty,
and the runtime log contains no ERROR/FATAL or unprepared-character-texture
message. Missing-asset warnings remain. A low-system-commit warning occurred
while loading; this is not evidence that the earlier OOM is fixed.

| Metric | Previous foundation run | Resource publication run |
|---|---:|---:|
| FPS | 41.96 | 46.73 |
| Mean frame interval, ms | 23.833 | 21.401 |
| p95 frame interval, ms | 32.901 | 26.307 |
| p99 frame interval, ms | 45.003 | 42.938 |
| CPU scene extraction, mean ms | 11.320 | 9.146 |
| CPU M2 extraction, mean ms | 6.245 | 4.817 |
| GPU WMO, mean ms | 4.617 | 4.539 |
| GPU RT lighting, mean ms | 3.289 | 3.287 |
| Peak sampled process working set, GiB | 5.093 | 5.133 |
| Peak sampled process private commit, GiB | 9.677 | 9.604 |

The observed FPS increase is 11.4%; this historical comparison does not isolate
code effects. Measured viewport changed from 2560x1369 to 2560x1392 (1.68% more
pixels), system memory conditions differ, and asynchronous resident populations
are slightly different: mean M2 104414 -> 104469, characters 440.7 -> 447.5.
The original pre-foundation baseline was 42.46 FPS. RT mode 3, MSAA 1, render
thread 1, settle 8 seconds and dev/no-LTO build are unchanged.

New M2 subphase means: candidate gathering 1.555 ms, emitters 1.606 ms, view
classification 0.766 ms, shadow lists 0.544 ms, pose publication 0.334 ms,
frame/resource acquisition 0.011 ms. These are parts of the M2 extraction
measurement, not additional frame costs. No older subphase baseline exists.
GPU-slot wait increased from 2.94 to 7.96 ms and producer queue wait from 0.62
to 2.64 ms; the faster producer now encounters more downstream waiting. These
wall measurements alone do not establish a GPU critical path, but WMO/RT GPU
costs remain largely unchanged and remain the next renderer targets.

Raw trace, CSV, both comparisons, runtime log and memory samples are preserved
in `build-dev/scratch/scene-replay/run-20261022-114001-489/`. No old client version
was rerun. Current-frame RT/G-buffer implementation is still pending.

## Remaining implementation

- [x] Finish foundation GPU snapshot/lifetime tests and guards.
- [ ] Record exact material/depth/blend contracts and choose G-buffer packing,
      MSAA resolve and device capability/memory budgets.
- [x] Connect primary WMO evaluated surfaces and RT-disabled lighting, with
      ordered blended runs, MSAA, slot lifetime and GPU image checks.
- [ ] Extend opaque/cutout G-buffer and material lighting to the other families;
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

## Evaluated WMO surfaces — 2026-10-22

Implemented the first connected primary-view G-buffer/lighting path, opt-in
with `WOWEE_WMO_SURFACES=1` and RT mode 0. Texture/POM/normal evaluation is
shared with forward WMO. Three attachments preserve material inputs; lighting
reads actual MSAA samples and retains the authored opaque/blended order.
The render thread records from the immutable WMO snapshot. Material capacity
is accumulated during the existing extraction traversal and upload occurs while
recording geometry: no additional model walk or copied draw list. Other subsystem
workers continue parallel recording. Per-slot upload ownership and shared-image
dependencies add no GPU idle or queue drain.

This is WMO only, not completion of the full pipeline. Current-frame RT,
receiver identity/motion/history, M2/character/terrain conversion, persistent
GPU tables and indirect submission are still pending. Reflections and blended
WMO use forward shading; RT-on and wireframe are explicitly rejected by the
opt-in path. Automatic resize memory-budget policy remains pending.

GPU checks use `tools/scene_render/check_wmo_surfaces.py`. The generated layer
fixture goes through the public WMO upload/placement API and exists only in
scene_render. A negative control moves opaque batches across a blended layer
and must visibly change the reference; the new pass must preserve the original.
The test includes alpha-cutout holes and MSAA coverage. Real ship/Dalaran cases
compare final lit images with bounded packing error, not only shader compilation.
Settings changed after extraction and source model removal preserve the queued
image; the following frame adopts the changes. Water reflections are included.

First isolated cost checkpoint (before final camera-relative reconstruction):
Dalaran WMO alone, saved investigation camera, 2560x1369, RT=0, MSAA=1,
FXAA, shadow quality 2, volumetrics 3, render thread + parallel recording,
dev/no-LTO, validation off. Each mode ran 240 frames; 230 GPU frames were
read back after warmup. No competing wowee/scene_render process at preflight.

| Metric | Forward | Evaluated surfaces |
|---|---:|---:|
| WMO GPU mean, ms | 2.80 | 2.68 |
| WMO GPU p95, ms | 2.96 | 3.15 |
| Whole GPU mean, ms | 4.58 | 4.20 |
| CPU renderWorld mean, ms | 0.25 | 0.49 |

This is one paired diagnostic comparison, not a measured game FPS improvement.
VSync limits this fixture near 60 FPS. New pass splits/primary recording raise
CPU recording cost; the main intended benefit depends on removing previous-frame
RT lookup, which is not connected yet. Raw inputs/logs/PNGs are preserved under
`../surface-cost-*` relative to this checkout. The saved full-client result remains
46.73 FPS from d440fce; no new full replay has run for this stage.

Final implementation check: native `wowee` and `scene_render` build passed.
Forced Khronos synchronization validation passed parallel/inline recording,
MSAA 1x/4x, day/night, FXAA, real Dalaran WMO, ordered layers and cutout,
water reflection, queued settings changes and source removal. Repeated MSAA
and FXAA target recreation preserves the final PNG byte-for-byte. The opt-out
WMO RT=3 source-mutation fixture also passes. Only the two known stale Epic
implicit-layer manifest messages are excluded. `sweep_guard` passed all available
checks (data/server/FrameXML-dependent checks explicitly skipped in this checkout);
recording and renderer-dependency guards remain at zero.

After camera-relative reconstruction and explicit target generations, only the
new mode was remeasured against the saved forward reference: WMO GPU mean
**2.85 ms**, p95 **3.22 ms**; whole GPU mean **4.36 ms**; CPU renderWorld mean
**0.44 ms**. This does **not** confirm a WMO speedup over forward (2.80 ms).
[Saved measurements and image comparisons](performance/dalaran-wmo-surfaces.json)
retain both the first checkpoint and final run, including the executable hash.
At 640x480 Dalaran mean RGB error is 0.062/255 for 1x and 0.062/255 for 4x;
ordered-layer error is 0.000059/255, max 1/255. The full-resolution check is
stored in the same JSON. There is no new measured full-client FPS result.

Next implementation step remains current-frame RT receiver/resolve/history,
then the other geometry families and persistent GPU submission. Do not enable
this partial WMO path as the default renderer or call the complete pipeline done.
