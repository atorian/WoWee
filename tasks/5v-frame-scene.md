# 5в — world draw lists extracted before recording

The frame must own the result of scene queries, not just the frame globals.
Keep extraction on the Present side and GPU preparation/recording on the Render
side. No render thread is enabled here.

Work and cost:

- Terrain: copied buffer/descriptor/range lists for colour, reflection, baking
  and depth passes; tile retirement must also cover an unrecorded snapshot.
- M2/WMO/characters: extraction owns visibility, poses, shadow candidates and
  particles; recording must not cull or gather from mutable instances.
- Water/grass: surface/population handles and simulation inputs in the frame.
- Sky/weather/effects/overlays: copied draw data, including selection heights.
- RT: ordered immutable scene changes extracted with the raster scene; GPU
  uploads, acceleration structures and history remain recording-owned.
- Extend record_scene_check and verify mutation/removal after extraction on the
  GPU, plus existing scene, RT and streaming tests and a short ordinary run.

This changes resource ownership across most world passes. Avoid copying entire
world meshes each frame: retain immutable model resources and copy visible
instance/draw data. UI copies and GPU-result delivery remain 5г and 5д.

Status: implemented and validated.

Implementation:

- `FrameInputs::scene` owns world draw data, bone poses, particles, selection
  heights and simulation inputs. Immutable model/group descriptions are retained
  by shared references; terrain and water draw lists copy GPU handles/ranges.
- CPU snapshot leases extend deferred destruction beyond already submitted GPU
  frames. An extracted frame can survive unloading its models or terrain;
  discarding it does not invent a GPU submission.
- RT producers enqueue ordered mesh/instance changes. Extraction retains changes
  until recording acknowledges them, so skipping an extraction loses no edits.
  Synchronization uses the requested frame mode, not last frame's GPU state.
- Grass population uploads are recorded copies, not immediate queue submissions.
  Character pose buffers belong to fenced recording slots and are independent
  of live character instances.
- Main and water-reflection passes now share their attachment dependency. The
  water removal test exposed incompatible render-pass dependencies; it failed
  Vulkan validation before the correction and passes afterwards.

Validation so far:

- The M2 removal-after-extraction GPU comparison failed before world draw lists
  were extracted. Five removal cases now pass: M2, WMO, character, terrain, water.
  Model resources are unloaded too; mutation cannot merely leave them resident.
- All thirteen frame-input/world-scene/retirement cases pass, including RT 1/2/3,
  FSR2, MSAA + FXAA, water with MSAA + FXAA and rain. GPU image tests force inline recording and run
  with the Khronos validation layer active.
- Eleven existing camera, lighting, fog and transport cases pass. RT mode sweeps
  at continent coordinates pass on hardware and compute tracing with no render
  validation errors. Five asset-reload/client-transition/transport cases pass.
- The expanded recording guard checks 65 functions and reports zero mutable
  world-scene reads; the frame-input guard checks 23 functions at zero.
  Full `sweep_guard` passed; its optional FrameXML runner cases remain skipped
  where that executable is not built.

The existing primary-buffer GPU-profiler validation issue in parallel recording
is documented in 5b. These strict image checks use the supported inline path;
passing them does not claim that issue is repaired. Ordinary interactive runs
must keep validation off. UI/composite extraction and complete GPU-result
mailboxes remain 5г/5д; the separate render thread remains 5е.

Ordinary client check:

- A 30-second Booty Bay lap completed without error log entries or a crash,
  using the full profile, RT 3, mist 0.65, view distance 3000, 2560x1369, MSAA 1x,
  validation off. Report: `build-dev/scratch/frame-scene/reports/benchmark-bootybay-20261020-191212.txt`.
- This startup/streaming run averaged 20.63 FPS; RT geometry builds remained
  queued throughout it. There is no matching pre-change run, so this is a
  streaming/lifetime check, not evidence of a performance improvement.
- The ordinary client was restored through `run.ps1` with the complete profile
  and server-extension data. It received seven characters and reached IN_WORLD
  on Isengard Tirion x2; validation remains off.
