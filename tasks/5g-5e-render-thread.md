# 5г–5е — interface snapshots, GPU results, render worker

Requested scope: complete 5г, then 5д, then 5е, retaining the single-threaded
comparison mode. The existing RT/star work is committed as 6262ccf; the unrelated
camera_controller.cpp diagnostic is excluded.

5г work and cost (large):

- Own ImGui vertices, indices, resolved texture IDs, clipping and draw offsets
  in the frame. Prepare atlas changes before extraction. Recording must not
  enter an ImGui context or invoke its texture-upload/backend frame functions.
  A small context-free Vulkan interface recorder owns fenced buffers.
- Extract minimap tile inputs and display settings, world-map tiles/overlays,
  and preview cameras/poses before recording. Retain resources/owners through
  recording and GPU completion. No archive reads or immediate uploads in a
  recording path; pending preparation retains its previous ready result.
- Include the map window's independent interface and composite in the snapshot.
- Verify held interface/preview/map snapshots against mutation and destruction,
  font growth, clipping and offsets, auxiliary window close/resize, and loading.

5д work and cost (medium):

- Publish numbered completed-render results through a synchronized mailbox:
  CPU/GPU timing, cull results, shelter and celestial visibility, captures.
- Consume on the game thread at a frame boundary. No game callback is invoked
  by recording or completion. Retain capture buffers until the consumer releases
  them, and distinguish render sequence numbers from game-frame numbers.

5е work and cost (large):

- Bounded two-frame handoff, condition-variable backpressure, exception delivery,
  drain/stop and inline mode (WOWEE_RENDER_THREAD=0).
- Render-side swapchain/acquire/submit/recreation and requested settings; game-
  side SDL events. Synchronize structural transitions without draining on every
  ordinary frame. Keep GPU resources and their immutable metadata alive.
- Audit producer/consumer shared caches, descriptors, mapped writes and statistics;
  fix the known secondary-pass profiler validation error before strict checks.
- Behavioural queue/lifetime tests, GPU image comparisons in both modes, complete
  sweep_guard, dev build and an ordinary launch with the full existing profile.
  Record performance as measured; enabling a thread alone does not prove a gain.

Status: 5г, 5д and 5е implemented and validated.

5г validation (2026-10-20): dev wowee, scene_render and wowee_editor build.
Sixteen GPU checks passed (owned interface/font pages, world snapshots, RT modes,
water/MSAA, rain, stars and AA). The final interface check and full applicable
sweep_guard passed. The opt-in native client probe passed preview rotation,
model/owner removal, real map replacement/owner removal, asynchronous minimap
textures/display changes, and auxiliary-context close with a held request.
Strict Khronos validation reports no VUIDs and zero surviving VMA allocations.
The probe first exposed and fixed a deferred preview-target leak; its later
minimap access violation was missing fixture world initialization, corrected
before accepting the test.

5д implementation notes:

- Separate producer CpuStageTimes (update, extraction, WMO/terrain loading) from
  VkContext's recording times. Results own timing label strings; render-graph
  string_views cannot cross a rebuild. Main consumes once at beginFrame.
- Completed packets need game sequence, recording sequence, GPU answer's actual
  submitted frame, and world epoch. Increment the producer epoch on map token
  replacement; reject old-map shelter/celestial answers. Collection should
  report whether fresh data was read (SkyBodies body mask, ShelterMap bool),
  rather than tagging a previous cached answer as a new GPU result.
- Move M2 verdict application from extract() to the result boundary; preserve
  scene identity and number each GPU verdict batch. No render-side instance writes.
- FrameSink scheduling and callbacks belong to the producer. Freeze capture
  engine/generation, optional PTS and indicator time in inputs; reserve PTS once.
  Publish ready pixel leases and drop notices through results. A shared pixel
  owner returns the readback buffer, retaining ScreenCapture until encoding ends.
  Capture start/end/flush drain queued recording. End must not explicitly destroy
  a pool whose buffers still have readers; obsolete generations are discarded.
- Replace post-present screenshots with queued requests, copies in the frame's
  own command buffer before presentation, and CPU PNG completion on the main
  boundary. UI says requested, then reports completion. Environment screenshot
  runs must wait for completion before quitting.
- Publish render statistics used by HUD/resource monitor too; RtScene::stats()
  currently walks consumer meshes and is unsafe to read from the producer.
- Before enabling parallel recording under strict validation, reserve timestamp
  slots in execution order and write inside secondary command buffers (including
  mark-only secondaries where needed). Primary commands inside a secondary-only
  render pass cannot contain timestamp/debug-scope commands. Keep per-pass GPU
  attribution; do not hide this by disabling profiling or validation.
- AuxSwapchain dirty/drawable sizes must be transferred from SDL/game events;
  acquire/rebuild must not query SDL. Freeze main drawable size in FrameInputs.
- Structural lifetime drains belong at real world/session teardown and GPU/UI
  setup boundaries, not every Transitions phase. Stop/drain before application
  shutdown destroys UI or world objects. WOWEE_RENDER_THREAD=0 still uses the
  identical extraction/results protocol and bounded worker API in inline mode.

5д validation (2026-10-20): dev wowee, scene_render and render_client_probe built. All 16 selected tests passed, including frame_results, world/RT/AA/weather snapshots and sweep_guard. The native client probe confirmed numbered results, producer-only capture callbacks, screenshot frame content and deferred completion; no VUIDs and zero surviving VMA allocations.

5е implementation and validation (2026-10-20):

- FrameWorker reserves at most two outstanding frames before extraction, uses
  condition-variable backpressure and a dedicated recording owner, delivers
  recording failures to the producer, and drains/joins before scene/UI teardown.
  WOWEE_RENDER_THREAD=0 executes the identical owned requests inline.
- Drawable extents cross in inputs; main/auxiliary recreation consumes those
  values. Vsync/dirty flags and published extents/MSAA are synchronized. Actual
  scene initialization, preview creation, map closure and surface loss drain
  accepted recording without serializing ordinary frames.
- GPU marks in secondary-only passes use inherited marker buffers allocated in
  execution order on the owner. Post-processing is marked after its pass closes.
  HUD GPU diagnostics cross through FrameResults; completed scene/UI owners are
  released when recording finishes. Profiles separate render-thread work and
  producer queue waits, including the benchmark's budget.
- Dev wowee, scene_render and render_client_probe built. All 17 selected tests
  passed, including frame_worker, frame_results and sweep_guard. Scene fixtures
  can run GPU recording on a dedicated owner via WOWEE_RENDER_THREAD=1.
- Strict native probe passed concurrent extraction/recording, exact owned UI
  image equality in inline/threaded modes, resize with two accepted frames,
  minimized submission skip, window restoration, captures and shutdown.
- Application::run completed a real lamppost/RT screenshot and clean shutdown
  in both modes through run.ps1. No unexpected Vulkan validation errors or
  surviving VMA allocations. These short runs do not replace stage 5ж's long
  sessions/crowd/flight A/B measurement; no FPS gain is claimed here.
