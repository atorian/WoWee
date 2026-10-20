# Bounded RT region and graphics distance control

Implement an independent RT distance, default 400 yards, adjustable 100–1500.
The outer 20% blends to raster lighting. Trace distance and occluder residency
are separate: retain bounds intersecting the receiver region expanded for GI
and swept toward the sun for shadow rays, including offscreen geometry.

Producer extraction owns region selection and immutable mesh/instance deltas.
Build only referenced meshes; retire unused BLAS/buffer ranges through fences.
Compact oversized triangle/node pools after residency shrinks, so reducing the
slider releases memory as well as reducing TLAS work. Use hysteresis for motion
and preserve bounded historical snapshots. All changes work in both backends.

Wire the schema control, persisted setting, renderer frame inputs, trace early
outs and composition weight together. Changing radius invalidates history.
Verify scene selection before/after movement, shared meshes, offscreen and
sunward occluders, snapshot mutation, real GPU memory reduction, and settings
persistence; run strict GPU tests, sweep_guard and the dev client.

Cost: source bounds and last-published residency on the producer; a bounded
selection pass, plus occasional GPU pool compaction when overallocated. CPU
source meshes remain available for re-entry without disk reads.

Status: implemented and validated; ordinary dev client launched.

Implementation and verification:

- `rt_distance` persists the graphics setting. `rtdistance` is the schema key on
  the Ray Tracing page; default 400, range 100–1500, 25-yard slider steps.
  The radius is the outer edge; blending starts at 80% of it. The frame owns
  both residency selection and the receiver distance, including history reset.
- Region selection uses transformed bounds, GI/AO reach, and a conservative
  sunward sweep including the solar cone. Retained placements have a 10%
  exit margin (at least 16 yards). A shared mesh stays until its last selected
  placement leaves. Dropped snapshots retain ordered residency deltas.
- BLAS retirement and pool compaction are fenced. Mesh table versions are
  immutable so earlier GPU frames keep the offsets of their original pools.
- The WMO radius regression reported two RT placements before implementation.
  The fixture must pin its camera near the receiver; automatically framing
  both near and remote models puts the camera outside the requested radius.
- On hardware RT, the isolated residency probe's RT/buffer allocation grows
  from 14,632,784 to 81,370,180 bytes, then falls to 12,868,040 after reducing
  the radius. On compute it grows from 18,813,280 to 36,749,200, then falls to
  17,047,456 bytes. These synthetic values verify release, not city savings.
- Both actual backends pass offscreen sun-caster retention, shared geometry,
  camera/instance movement, skipped extraction and older-snapshot checks.
  The actual production lookup gives weights 1, 0.5 and 0 through the fade;
  out-of-radius tracing returns neutral direct/ambient and zero GI correction.
  Strict Vulkan validation reports no render errors.

Probe launch pitfalls corrected: remove WOWEE_RT_FORCE_SOFTWARE entirely for
hardware (its presence forces compute, even when set to 0); use a typed native
argument array in PowerShell; assert the backend and scenario marker in logs.

Final checks: RT region/scene regression, settings schema/layout and RT BVH
cases pass. Existing frame-image cases (M2/WMO/character/terrain/water, MSAA,
FSR2, weather and mode changes) pass. Both real backends are asserted in the
probe logs and run with Khronos validation. `sweep_guard` passes; its optional
FrameXML restart test is skipped because the runner/interface fixture is absent.

Ordinary client: launched via this worktree's run.ps1 with the complete profile
and server-extension data, validation off, hardware RT. The client reached
IN_WORLD and saved `rt_distance=400` with mode 3. During world loading, the
resource log reported about 298–302 MB in the ray tracing category. The scene
was still streaming, so this is not a controlled comparison with the earlier
2.3–3.1 GB Dalaran run and no FPS improvement is claimed from it.
