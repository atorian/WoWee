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
      lighting for all families; WMO opaque/cutout receiver worklist, rigid
      motion/identity, base history and centralized filtering are implemented.
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

## WMO worker recording — 2026-10-22

Moved evaluated WMO command recording back to a frame worker. Resource
preparation stays on the render owner; the worker produces contiguous authored
opaque/blended secondary runs in the existing snapshot traversal. The owner
executes those runs and their lighting boundaries without walking models.
Frame-slot command pools are reset after acquisition and reused, and repeated
pipeline/frame/material bindings are suppressed. A WMO worker failure cannot
submit a partial plan; all workers join before propagating it.

Native `wowee` and `scene_render` builds passed. Forced Khronos synchronization
validation passed serial/parallel recording, ship day/night, 1x/4x MSAA, FXAA,
repeated target recreation, interleaved opaque/blended/cutout layers with the
reordering negative control, Dalaran, water reflection, queued material settings
and source removal. The ordinary forward RT=3 mutation fixture also passed.
All nine comparable captures (including full-resolution Dalaran) have exactly
zero changed RGB channels against the saved previous surface implementation.
Recording and dependency guards remain at zero. `sweep_guard` passed every
available check; data/server/FrameXML-dependent checks were explicitly skipped
where their fixtures or executable are absent in this checkout.

Only the changed version was measured, using the saved RT=0 isolated Dalaran
WMO scene, 2560x1369, MSAA=1, FXAA, shadow quality 2, volumetrics 3, render thread
and parallel recording, dev/no-LTO, validation off, 230 GPU samples after warmup.

| Metric, ms | Previous surface path | Worker surface path |
|---|---:|---:|
| CPU renderWorld mean | 0.44 | 0.38 |
| CPU renderWorld p95 | 0.48 | 0.53 |
| WMO GPU mean | 2.85 | 2.85 |
| WMO GPU p95 | 3.22 | 3.63 |
| Whole GPU mean | 4.36 | 4.45 |
| Whole GPU p95 | 4.86 | 5.21 |

The CPU mean improved by about 14%, but tails did not improve and the single
run does not establish GPU speedup. CPU renderWorld includes waiting for workers;
this isolated scene provides little other geometry work to overlap. It is not a
full-client FPS result. Previous full-client measurement remains 46.73 FPS.
[Saved worker measurement](performance/dalaran-wmo-workers.json) includes the
scene, binary hash, raw log location and image comparisons. Default rendering
is unchanged; current-frame RT and the remaining full-pipeline stages are pending.

## Current-frame opaque WMO RT — 2026-10-22

Connected current evaluated WMO receivers to hardware/software tracing in RT
modes 1–3. A half-resolution representative and explicit GPU worklist cover
current samples before material lighting. Sun/AO/GI share the existing estimator;
AO and GI share one closest-hit traversal. The consumer performs a direct light
load, with no previous-image recovery search. The acceleration scene is prepared
once before world rendering. WMO placement history and immutable batch identity
are captured during existing snapshot/recording walks; there is no new model
walk. Per-slot uploads and queue dependencies retain the threaded frame contract.

The first test exposed reversed derivative normals: unoccluded receivers became
dark. The test failed before orienting geometric normals toward the visible side
and passes after the fix on both tracing backends. GPU tests also cover missing
half-resolution receivers, same-depth MSAA samples with different normals,
uncovered samples, actual temporal accumulation, lifetime rejection and finite
GI after a mode change. Existing translated/swept RT probes pass on both backends.

Forced Khronos synchronization validation passed serial/parallel current RT,
1x/4x layers, real Dalaran, queued source removal with 4x MSAA + FXAA, and
current-RT MSAA/FXAA target recreation with frames in flight. RT-off
compatibility passed seven cases including ordered transparent/cutout layers,
night and repeated target recreation. Every available sweep is within its ceiling;
unavailable data/server/FrameXML fixtures are explicitly skipped. The default-off
`WOWEE_BUILD_RT_PROBE` owns the native test and its shader.

Measured only the changed implementation against the saved Dalaran WMO GPU
investigation: 2560x1369, RT3, MSAA1, FXAA, shadow quality 2, volumetrics 3,
dev/no-LTO, validation off, 230 GPU samples after warmup. FIFO limits this fixture;
use GPU timestamps, not its displayed FPS. No competing renderer was present at
preflight. The user authorized stopping PID 28572, but it was already absent when
the stop/restore wrapper ran, so that wrapper stopped/restarted no client.

| Metric, ms | Saved forward RT3 | Current WMO RT3 |
|---|---:|---:|
| WMO GPU mean, including new trace/filter | 7.62 | 6.55 |
| WMO GPU p95 | 11.74 | 7.05 |
| Retained legacy RT mean | 2.42 | 2.68 |
| WMO + both RT costs, mean | 10.04 | 9.23 |
| Whole GPU mean | 11.42 | 10.72 |
| Whole GPU p95 | 15.31 | 11.34 |
| CPU renderWorld mean | 0.35 | 0.47 |

Combined WMO/RT mean is 8.1% lower and whole GPU mean 6.1% lower; CPU recording
is higher. This is one historical comparison, not controlled proof of a game FPS
increase. A tiled filter reduced the first current-RT WMO result from 6.78 to
6.55 ms. Its full-resolution image differs in only 20 of 10,513,920 RGB channels,
each by 1/255; the ordered-layer captures are identical. No old binary was rerun.
[Report, hashes, memory accounting and raw paths](performance/dalaran-wmo-current-rt.json).

This remains an opt-in WMO stage. Blended/reflected WMO and other geometry still
need the legacy RT pass. Extra RT storage is 147.12 MiB at 1x or 347.66 MiB at 4x
for this viewport, beyond G-buffer storage; peak resize budgeting is unfinished.
Only base representatives have temporal history, and opaque-run boundaries still
cause fullscreen classification/filter scans. Sparse work dispatch and conversion
of remaining consumers are the next GPU targets. Full-client replay has not been
repeated: the last completed result remains 46.73 FPS from d440fce.

### Sparse WMO surface RT checkpoint (2026-10-22)

Coverage and base tracing now share a 16x16 tile dispatch. The GPU emits bounded
occupied/extra tile lists; additional rays and spatial filtering visit those
lists through indirect dispatch. Empty material keys exit before depth/normal
reconstruction. This adds no producer/model walk and keeps ordered opaque runs,
worker recording, per-slot history and fence-based retirement intact.

RT scratch drops from 20 to 16 bytes per full-resolution sample plus eight bytes
per screen tile. Temporal records drop from 48 to 40 bytes: lifetime identity and
float32 position stay intact, only the historical normal uses 12-bit octahedral
coordinates. Capable GPUs filter 16x16 outputs from a shared 24x24 tile; devices
with smaller limits retain the tested 8x8 implementation. At 2560x1369 with two
history slots, logical added RT buffers fall from **147.12 to 120.48 MiB at 1x**
and **347.66 to 280.91 MiB at 4x**. Surface images, material uploads, allocator
rounding and peak resize residency are additional.

Only the new version was measured against the saved 3bdf782 report, using the
same Dalaran WMO scene/settings and 230 GPU samples. There were no competing
wow/wowee/scene_render processes in 55 monitoring samples; this does not control
browser/OS GPU work or clock variation. The specifically authorized original
WoW PID was already absent when the stop/restore wrapper ran.

| Metric | Saved current WMO RT | Sparse WMO RT |
|---|---:|---:|
| WMO incl. surface RT GPU mean / p95, ms | 6.55 / 7.05 | 6.70 / 7.08 |
| Retained legacy RT GPU mean, ms | 2.68 | 2.70 |
| Whole GPU mean / p95, ms | 10.72 / 11.34 | 10.87 / 11.24 |
| CPU renderWorld mean / p95, ms | 0.47 / 0.64 | 0.72 / 1.11 |

**No FPS increase is established.** GPU mean is 1.4% higher and CPU recording is
also higher in this run; CPU causality is not established. This is a memory
reduction checkpoint. A separate classification pass and explicit filter-loop
unrolling did not improve the result and were removed. The fixture is FIFO
limited; its approximately 60 FPS is not a full-client performance result.

GPU probes pass on hardware/software tracing, filter16/filter8 and MSAA1/MSAA4,
including cold sparse coverage, moving coverage through an empty run and odd
partial edge tiles. Khronos synchronization validation passed the serial/parallel
scene checks, interleaved opaque/blended runs, MSAA/FXAA target recreation and
queued source mutation. The strict scan found no validation errors beyond the
two known missing Epic overlay manifests. All available architectural sweeps
passed; missing data/server/FrameXML fixtures were skipped. Layer captures match the saved images;
full-resolution Dalaran differs in 210 of 10,513,920 RGB channels (maximum 12/255).
[Report, hashes, diagnostics and raw paths](performance/dalaran-wmo-sparse-rt.json).

The pipeline remains opt-in and incomplete: base tracing still visits screen
tiles once per opaque run, extra receivers lack temporal history, and blended
WMO, reflections and other geometry still require legacy RT. Converting those
consumers is necessary before removing that duplicated lighting path. No new
full-client replay was run; the last result remains 46.727 FPS at d440fce.

### Opaque/cutout M2 current surfaces (2026-10-22)

Previous work was already committed as ccdc66d before this stage began. The new
`WOWEE_MODEL_SURFACES=1` path (earlier WMO flag retained as an alias) connects M2
to current receiver lighting. `Renderer` owns one `SurfacePass`: WMO/M2 share
surface images and RT scratch/history, while their workers own disjoint material
ranges and command pools. Both retain snapshots, instancing, existing execution
order and frames in flight. No additional scene/model traversal is introduced.

Texture stages, UV animation, alpha-to-coverage, wind and skinning run in the
existing geometry shader. Both forward and evaluated paths use one M2 lighting
function. Each instanced batch supplies a material base; the vertex invocation
adds its instance index. Blended batches keep forward lighting and their order,
including the water boundary. A shared lifetime namespace prevents WMO and M2
history collisions. Rigid M2 history uses the previous rendered transform;
deformations invalidate it until deformation-aware correspondence is implemented.
Existing RT caster geometry publication is unchanged.

Builds of wowee, scene_render and rt_probe pass. GPU checks passed for:
- Different interior lighting on instances of the same model, two texture
  stages with additive contribution, cutout coverage and blended layers.
- Mixed WMO/M2, serial/parallel recording, hardware/software RT, MSAA1/MSAA4,
  and target recreation between 4x/1x/4x with FXAA.
- Real wind-animated foliage and a skinned wolf. RT-disabled forward/surface
  mean RGB-channel errors are 0.00283/255 and 0.00168/255 respectively.
- Queued M2 and WMO snapshots after removing their scene sources, with MSAA4
  and FXAA; only the next extraction changes pixels.
- Native current-receiver/history tests and every available architecture sweep.

Strict scanning of 44 GPU logs found no error except the known missing Epic
overlay manifests. Serial and parallel captures match exactly. The surface
geometry/RT buffers remain one shared set (133.69 MiB color and 120.48 MiB RT at
2560x1369, MSAA1); material upload/command capacity is additional. No process or
resize peak-memory claim is made.

Measured only the new implementation, dev/no-LTO, RT3, MSAA1, FXAA, 2560x1369,
230 GPU samples, validation off. Both runs had continuous competing-renderer
checks (36 and 80 samples, none present). OS/browser GPU load and clocks were
not controlled. FIFO limits presentation, so these are GPU timestamp results.

| Metric | Saved WMO control (ccdc66d) | New WMO control | New WMO + M2 doodads |
|---|---:|---:|---:|
| GPU mean / p95, ms | 10.87 / 11.24 | 11.01 / 11.46 | 23.32 / 27.15 |
| CPU renderWorld mean / p95, ms | 0.72 / 1.11 | 0.48 / 0.66 | 3.88 / 5.78 |
| WMO GPU mean, ms | 6.70 | 6.81 | 7.80 |
| M2 GPU mean including surface RT, ms | 0 | 0 | 9.31 |
| Retained legacy RT mean, ms | 2.70 | 2.71 | 3.10 |
| M2 draws in captured frame | 0 | 0 | 3052 |

The prior WMO CPU increase did not reproduce; this does not establish its cause.
The new mixed scene is a first saved baseline, not an optimization comparison
against WMO-only. **No FPS increase is confirmed.** Full-client replay did not
start: free system commit was 8.51 GiB, below the 11 GiB guard based on the prior
9.60 GiB peak plus headroom. A user reply to the memory question is pending.
Last full-client result remains 46.727 FPS at d440fce.

[Settings, hashes, distributions, validation and raw paths](performance/dalaran-m2-surfaces.json).
This completes the opaque/cutout M2 connection, not the full pipeline. Character,
terrain, reflection and blended consumers still require legacy RT. The separate
WMO/M2 runs cannot be merged across the intervening selection/character draws
without first converting those consumers and preserving their ordering semantics.


### Full replay on current data (2026-10-22, 0800842)

Memory became available (35.18 GiB free commit). The original recording rejected
its changed manifest: 32,824,155 captured bytes versus 37,713,221 current bytes.
The original recording is preserved. A separate derived scene updates only the
manifest signature; events/settings and all 24 archive signatures are unchanged.
This supersedes the pending-memory status above, but does not establish a
controlled comparison against d440fce. Provenance and hashes are in the report.

760 samples completed normally, 759 producer intervals. Hardware RT3, MSAA1,
FXAA, render thread 1, VSync off, dev/no-LTO. Final viewport 2560x1369, versus
2560x1392 historically. Streaming continued after warmup; missing-asset warnings
and 3040 residency-counter mismatches remain. No ERROR/FATAL occurred.

- FPS **29.136**, frame interval mean/p95/p99 **34.322 / 63.233 / 99.104 ms**.
  Historical result: 46.727 FPS with different resources/loading conditions.
- GPU M2 including surface RT **10.273 ms mean / 12.848 ms p95**;
  WMO **6.660 ms mean**; retained legacy RT **2.832 ms mean**.
- CPU extraction **12.239 ms mean / 16.738 ms p95**; render worker wait
  **4.070 ms mean**. Nested timings/waits are not additive CPU work.
- Peak private bytes **15,513,673,728 (14.45 GiB)**; peak working set
  **9,526,714,368 (8.87 GiB)**, sampled every 500ms including loading.

Investigate the M2 surface/RT cost on this saved workload before claiming gains
from converting more families. Character, terrain, reflection and blended
conversion remains incomplete. The earlier 11 GiB replay guard no longer gives
headroom for the observed current-data workload.

[Full distributions, provenance and raw paths](performance/dalaran-m2-surfaces-replay.json).


### Longer warmup replay (2026-10-22, same 0800842 binary)

User requested another run to reduce loading effects. Same executable hash and
current-data scene, settle increased from 8 to 60 seconds (budget 120 seconds).
Warmup finished without the busy-budget warning, with stable resident counters.
All 760 source samples completed; no ERROR/FATAL or trace error, normal exit.

| Metric | Previous settle 8s | Settle 60s |
|---|---:|---:|
| FPS | 29.136 | 30.670 |
| Frame interval mean, ms | 34.322 | 32.605 |
| Frame interval p95, ms | 63.233 | 55.679 |
| Frame interval p99, ms | 99.104 | 73.691 |
| GPU M2 incl. surface RT mean, ms | 10.273 | 10.330 |
| GPU WMO mean, ms | 6.660 | 6.732 |
| CPU extraction mean, ms | 12.239 | 10.692 |

This is a 5.3% observed FPS increase without a code change, with improved frame
tails. It does not remove M2's GPU cost. Playback still logged 25 DDS loads for
resources encountered during the recorded sequence; initial readiness is not
whole-route preloading. Missing assets and residency mismatches remain.
Actual viewport was 2560x1392 versus 2560x1369 previously (1.68% more pixels).
Cache/system differences were not isolated. All old reports are preserved.

[Full metrics and warmup evidence](performance/dalaran-m2-surfaces-warm60.json).


### HD asset cache audit (2026-10-22)

User confirmed the current resources come from an HD client. The warm60 log
contains 947 successful DDS reads of 325 paths (622 repeats), representing
1,409,206,640 logical bytes versus 551,249,072 unique file bytes at current sizes.
All 25 measured DDS reads had already occurred during warmup; the earlier
"new resources" explanation was too broad. These are not physical disk bytes,
and some repeated reads serve distinct decoded/normal-map output forms.

GPU texture cache was last sampled at 1.96/4 GiB with no full warning. Raw file
cache ended at 1022 MiB and 36% hits, but that metric excludes uncached DDS and
cannot prove thrashing. The concrete avoidable-work candidate is worker-side
preparation: terrain jobs deduplicate textures per tile, WMO normals are prepared
before later GPU-cache checks, and DDS reads bypass raw-file retention. Cache
hits on upload therefore do not prevent earlier reads, analyses and allocations.

Prioritize bounded shared immutable preparation/in-flight jobs by asset generation,
path and output recipe, with safe resource pinning and per-key cost counters.
Do not increase memory budgets blindly or promise this fixes M2's GPU RT cost.
[Evidence and limitations](performance/dalaran-hd-cache-audit.json).


### Bounded M2 surface runs and shared CPU preparation (2026-10-22)

M2 records conservative screen rectangles while writing the existing instance
snapshot, unions them per authored geometry run, and limits geometry clears,
RT dispatch and lighting to that rectangle. Rectangles align to global 16 px RT
tiles; receiver reads reject the outside before fetching, including filter halos.
Bones, foliage/cloth/water deformation and eye-plane crossings keep full extent.
No world traversal or reordering of intervening forward draws was added.
`WOWEE_SURFACE_FULL_EXTENT=1` provides a correctness reference;
`WOWEE_SURFACE_RT_PROFILE=1` reports run counts/area and first run GPU components.

AssetManager shares immutable prepared textures across tile workers with a
256 MiB LRU and per-key in-flight joining. Stored and RGBA forms reuse the source;
asset generations detach obsolete in-flight work. Terrain/WMO normal preparation
has a separate 128 MiB LRU keyed by source generation, recipe and output format.
Queued uploads pin results. Mutable legacy callers receive independent copies
with invalidated alpha analysis. Retention budgets do not bound outstanding pins;
filesystem hot reload and the remaining mutable entity/spell APIs were not redesigned.

Validation: 157 cache assertions/13 cases pass, including fail-before reproduction
(three source reads instead of one), concurrent invalidation, eviction pins and
exception propagation. Bounded/full and serial/parallel images match exactly in
mixed WMO/M2, MSAA 1/4, target recreation, eye-plane and software RT fixtures.
Hardware/software SurfaceRt probes pass. Synchronization validation reports no
new errors; only the two known absent Epic overlay manifests were excluded.
`sweep_guard` passes its available checks; FrameXML probes without their runner
are explicitly skipped. Client, scene_render and rt_probe builds pass.

Isolated Dalaran: 26 geometry/25 forward M2 runs remain, but run area falls from
91,120,640 to 68,198,656 pixels (-25.2%). M2 including current RT falls 9.26→8.32 ms;
whole GPU 22.99→22.67 ms. The output matches the saved image bit for bit.
[Diagnostic measurements](performance/dalaran-m2-regions.json).

Full current-HD replay, 60 second settle, 760 samples:

| Metric | Saved warm 60 | New cache + regions |
|---|---:|---:|
| FPS from producer intervals |30.670|32.985|
| Frame mean / p95 / p99, ms |32.605 /55.679 /73.691|30.317 /49.874 /68.807|
| GPU M2 incl. RT mean, ms |10.330|9.261|
| CPU extraction mean, ms |10.692|10.098|
| DDS reads / repeated reads |947 /622|500 /175|
| Repeated logical DDS bytes, MiB |818.21|228.35|
| Peak private bytes, GiB |14.16|14.31|

The observed FPS increase is 7.5%, with a smaller viewport in the new full run
(2560 x 1369 versus 2560 x 1392, 1.65% fewer pixels) and 418 initial characters versus 419.
It is not a controlled code only percentage. Twenty measured DDS reads still
repeat warmup paths after eviction. Memory did not decrease: the bounded caches
trade about 151 MiB additional peak private bytes for fewer repeated preparations.
[Full replay, memory, cache counters and caveats](performance/dalaran-m2-regions-cache-replay.json).

The complete pipeline is still unfinished: remaining consumers use legacy RT,
and deformed M2 and ordered forward boundaries still carry substantial GPU cost.
