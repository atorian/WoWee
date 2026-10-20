# 5b — frame values extracted at the end of Present

The game hands an owned FrameInputs to recording: camera, character, sampled
lighting, sky/weather/wind, fog, view flags, local lights and requested graphics
settings. Jitter modifies only a recording-owned camera copy. RT uses that same
camera and light; its temporal history follows submitted frames rather than game
updates. No render thread is enabled at this stage.

Work:

- [x] Reproduce mutation after extraction changing the kept GPU image.
- [x] Convert frame-global inputs in every pass and post-processing.
- [x] Capture at the end of the client's Present, including non-world views.
- [x] Guard recording against live frame-global reads.
- [x] Verify extraction isolation, RT modes, AA, fog/weather and existing scenes.
- [x] Dev build and client launch through run.ps1.

Evidence:

- Before conversion, changing the source camera after extraction changed the
  GPU image (`build-dev/scratch/frame-inputs/before-*.png`).
- Six GPU regression cases now keep exactly the same PNG after source mutation
  and change on the next extraction: RT 1/2/3, FSR2, MSAA + FXAA, and rain.
  Recording also leaves the live camera's jitter untouched. Vulkan validation
  reports no rendering errors for these cases (the machine has two stale Epic
  implicit-layer JSON registrations, reported separately by the loader).
- Eleven existing camera, light, fog and transport scene tests pass. RT mode
  sweeps at continent coordinates pass on hardware and compute tracing, with
  Vulkan validation. `sweep_guard` passes; the new input guard checks 23 recording
  functions and reports zero live frame-global reads.
- Lamp flicker now uses extracted frame time. LightingManager receives elapsed
  update time explicitly, so fixed-step scene runs have the same light fades.
- Scene PNG readback uses the existing fenced FrameSink capture before present,
  replacing the tool's invalid read of an already presented swapchain image.
- The dev client was rebuilt and launched through the worktree's run.ps1 with
  the complete existing profile and server-extension data; seven characters
  were received. The ordinary client is left running without validation.

Performance verification limitation:

The 240-second Booty Bay diagnostic completed at view distance 3000, RT 3 and
mist density 0.65, but validation found existing timestamp/debug-label commands
written to the primary buffer inside a secondary-only render pass. This run's
2.32 FPS is not a performance baseline. The profiler error requires a separate
fix; a before/after performance conclusion needs the same scene/settings without
validation. After restoring ordinary operation, the live Dalaran log reported
about 23–29 FPS; that is observational, not a controlled comparison.

Geometry snapshots, RT scene change packets, draw-list extraction and ImGui
copies remain stages 5в/5г of the supplied plan, not part of 5b.
