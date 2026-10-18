# Performance Baseline - WoWee

How the client's frame time is measured, and the numbers it was measured at.
Measure before optimising, and re-run the same benchmark after each change:
a number from another route, another resolution or another build preset is
not comparable.

## The benchmark (`WOWEE_BENCHMARK`)

A fixed camera flight through a city, the same on every run, with no realm,
no login and no character of yours:

```
cd build-dev/bin
WOW_DATA_PATH=D:/games/WoWee/Data WOWEE_BENCHMARK=dalaran ./wowee.exe
```

The client enters the route's map offline (the Mulgore fixture's world
entry), waits until the world is in view and terrain has stopped streaming in
(at least `WOWEE_BENCHMARK_SETTLE` seconds, 10 by default, at most 60), then
flies one lap of the route in `WOWEE_BENCHMARK_SECONDS` (60 by default), writes
its report and closes. Nothing before the flight - loading, streaming, the
first pipelines being built - is in the numbers.

Routes (`src/core/benchmark_run.cpp`), each a 12-point lap round the centre,
looking at it:

| route | where | hour | lap |
|---|---|---|---|
| `dalaran` | Dalaran's centre (571; 5804, 624, 647) | 12:00 | r 100, 35 above |
| `stormwind` | Trade District (0; -8833, 628, 94) | 16:30 | r 90, 30 above |
| `orgrimmar` | Valley of Strength (1; 1676, -4315, 61) | 09:00 | r 70, 30 above |

The report goes to `logs/benchmark-<route>-<YYYYMMDD-HHMMSS>.txt` beside the
client (`WOWEE_BENCHMARK_DIR` overrides) and its summary line to the log:

- the device, swapchain size, vsync, frame cap and MSAA it ran at - compare
  only runs that agree on these;
- `summary:` fps, frame time p50 / p95 / p99 / max, and the frames over
  8.3 / 16.7 / 33.3 ms (missed 120 / 60 / 30 fps), with their share;
- the frame budget of the flight: every CPU stage of the frame, costliest
  first, with average, p95 and worst, and the parts of a stage indented under
  it (below);
- the GPU's passes, averaged over every frame read back;
- the scene it flew over: M2 / WMO instances, characters, terrain tiles
  (average and most).

`WOWEE_BENCHMARK_MSAA=1|2|4|8` runs at that sample count without changing the
saved setting - what the scene costs in pixels against what it costs in
geometry. The scene counts include terrain chunks drawn and the M2 and WMO
draw calls.

Vsync and the frame cap are the player's settings and the run keeps them; a
CPU-bound frame is not affected, but a fast one is held to the display.

## The frame profile (`WOWEE_FRAME_PROFILE=1`)

Every 10 seconds, at warning level (info without the flag), the same frame
budget as the benchmark's over the last window:

```
frame budget over 10s: 412 frames, 41.10 fps; frame p50 23.80 / p95 31.20 / p99 44.90 / max 61.30ms (avg 24.33), over 8.3ms: 412, over 16.7ms: 398, over 33.3ms: 7
  Render: 15.10ms/frame avg, p95 19.80, worst 41.00ms, 62% of the window
  beginFrame: 9.80ms/frame avg, p95 13.10, worst 30.20ms, 40% of the window
    wait for GPU: 4.10ms/frame avg, ...
    graph: 2.30ms/frame avg, ...
      shadow_pass: 1.70ms/frame avg, ...
    per-frame UBO: 0.90ms/frame avg, ...
      local lights: 0.70ms/frame avg, ...
  ...
  GPU, 410 frames read back: 12.10ms/frame avg, p95 14.00, worst 19.90ms across 24 passes
    shadows: 1.10ms avg, p95 1.30, worst 2.20ms
  ...
```

(Illustrative layout; the numbers are below.) The client's phases (`Input`,
`Game`, `Render`, ...) and stages (`gameHandler->update`, `beginFrame`,
`renderWorld`, `endFrame`, ...) are at the top level. The renderer's own calls
are broken down by `CpuStageTimes` (`include/rendering/cpu_stage_times.hpp`):

| part | what it is |
|---|---|
| `beginFrame/wait for GPU` | the CPU idle on the frame slot's timeline/fence - the GPU's previous use of the slot not finished. Large here and the frame is GPU-bound |
| `beginFrame/acquire image` | `vkAcquireNextImageKHR` - the presentation engine holding the frame back |
| `beginFrame/wait for uploads` | the transfer-queue upload wait, when one is needed |
| `beginFrame/retire slot` | the slot's deferred deletions and the GPU timestamps read back |
| `beginFrame/targets and uploads` | settings applied between frames, render targets, finished upload batches |
| `beginFrame/frame bindings` | descriptor writes for the slot, the sun's occlusion queries |
| `beginFrame/per-frame UBO` (`/local lights`) | the frame's uniform block and the M2/character point lights |
| `beginFrame/sky set`, `M2 cull`, `grass` | the sky's set, the M2 and grass cull dispatches |
| `beginFrame/graph build`, `beginFrame/graph/<pass>` | the frame graph, and each pre-pass recorded (`shadow_pass`, `volumetric_fog`, `sea_bed`, `ocean_waves`, `reflection_pass`, `minimap_composite`, `offscreen_composites`) |
| `beginFrame/sky capture` | the sky background drawn before the scene pass |
| `rendererUpdate/...` | `Renderer::update` by phase: lighting, `weather and wind` (`wind field`, `water waves`), terrain streaming, sky/effects, characters, `M2 animation wait`, character emitters, cleanup |
| `presentWorldToRenderer`, `followRendererUpdate` | the client's work either side of the renderer's update in the Present phase |
| `renderWorld/prepare WMO`, `prepare M2`, `prepare characters`, `dispatch`, `wait for workers` | the main thread's share of the parallel recording, and its wait for the slowest worker |
| `endFrame/post-process`, `interface`, `submit`, `present` | recording the post-process and the interface, `vkQueueSubmit`, `vkQueuePresentKHR` |

So `wait for GPU`, `acquire image` and `present` are waiting; everything else
is the CPU recording the frame.

The GPU section averages each pass over every frame read back in the window -
it used to show the last frame only. A pass's time is the gap from the mark
before it to its own; every frame-graph pass now ends on a mark, so the
minimap's and the composites' cost is no longer read as the shadows'. On
MoltenVK, which resolves a timestamp to its render pass, the marks inside one
pass collapse onto the first; the driver is identified at device selection and
the report says so there and only there. A pass that reads near zero on
another driver is a pass that cost nothing (`world total` is a mark right
after the last world pass, `contact shadows` with them off).

## Captures: debug labels

Each pass is a `vkCmdBeginDebugUtilsLabelEXT` region of its own name whenever
the loader offers `VK_EXT_debug_utils` - not only under validation - so a
RenderDoc or Nsight capture of an ordinary build groups its commands by pass,
and images and buffers carry their names. `WOWEE_VK_DEBUG_LABELS=0` turns it
off. In code, `GpuPass` (a label and a timestamp mark of one name) and
`GpuScope` (a label only) in `include/rendering/vk_context.hpp`.

## Tracy

Optional, and free when off: without `WOWEE_ENABLE_TRACY` every Tracy macro
expands to nothing and `extern/tracy` is not needed. Tracy is not vendored;
fetch the client, build a tree of its own and connect the profiler of the
same release:

```
tools/fetch_tracy.sh                 # v0.11.1 into extern/tracy
cmake --preset dev -B build-tracy -DWOWEE_ENABLE_TRACY=ON
ninja -C build-tracy wowee
```

Tracy's GUI or `tracy-capture` v0.11.1 (https://github.com/wolfpld/tracy/releases)
then connects on port 8086. What it shows:

- CPU zones: the `ZoneScopedN` markers (`Application::run`, `Renderer::beginFrame`,
  `Renderer::renderWorld`, `M2Renderer::update`, `TerrainManager::update`,
  `AssetManager::loadTexture`, ...) and every `CpuStage` part above, by name;
  `FrameMark` at the top of each frame.
- GPU zones on the graphics queue: every `GpuPass` / `GpuScope` - the same
  names as the debug labels - through TracyVk, collected each frame in
  `VkContext::beginFrame`.

## scene_render timing

`tools/scene_render` draws a scene without the client; with timing it prints
every frame's wall time, its renderer calls and their three largest parts, the
GPU time of the last frame read back, and then the frame budget of the frames
after the warm-up (the first 10, or a third):

```
ninja -C build-dev scene_render
SCENE_RENDER_TIMING=1 tools/scene_render.sh tools/scene_render/scenes/dalaran.json build-dev/scratch/dalaran.png
```

A renderer change can be timed on a fixed frame this way in seconds, before a
benchmark run of the client.

## Other switches

- `WOWEE_PASS_ABLATION=1` - switches the world passes (terrain, grass, WMO,
  M2, ground clutter, far doodads, characters, sky, shadows) off one at a time
  for a few seconds each, with a baseline at both ends, and logs what the
  frame did without each. Stand still outdoors until the table is logged.
- `WOWEE_SINGLE_THREAD_RECORD=1` - records the world inline on the main thread
  instead of into parallel secondary command buffers, so each pass gets its
  own GPU mark (grass is otherwise counted inside terrain).

## Baseline

First run of `tools/benchmark.sh <route> 60`, 2026-10-18, `build-dev`
(RelWithDebInfo, no LTO) at commit `b0f393e` plus this measurement work, no
other client running. AMD Radeon RX 7700 XT, 2560x1392 swapchain (1369 for
Orgrimmar - the window's size), vsync off, frame cap 120, MSAA 8x, shadow
distance 500, GPU timestamps and pass labels on, no Tracy.

| route | frames | fps | p50 | p95 | p99 | max | > 16.7 ms | > 33.3 ms | GPU avg / p95 |
|---|---|---|---|---|---|---|---|---|---|
| dalaran | 1897 | 31.6 | 30.4 | 37.4 | 41.9 | 312.7 | 100% | 26.3% | 29.9 / 35.6 |
| stormwind | 2129 | 35.5 | 27.2 | 36.5 | 42.6 | 258.5 | 100% | 11.3% | 26.1 / 32.4 |
| orgrimmar | 2618 | 43.6 | 22.0 | 29.7 | 37.1 | 534.2 | 94.9% | 2.4% | 20.8 / 25.7 |

(ms; no frame of any route made 120 fps.) The scene: Dalaran 91.5k M2 instances
of 3680 models and 679 WMO instances, Stormwind 97.4k / 1754 / 315, Orgrimmar
95.3k / 1655 / 329, 110-113 terrain tiles each.

Where Dalaran's 32 ms goes, per frame on average (p95 in brackets):

| CPU | ms | | GPU | ms |
|---|---|---|---|---|
| `beginFrame` | 12.4 (18.4) | | terrain (with grass) | 11.7 (14.3) |
| - `graph/shadow_pass` recording | 6.5 (8.9) | | wmo | 8.6 (11.3) |
| - `wait for GPU` | 4.5 (10.8) | | m2 | 3.0 (4.9) |
| - `M2 cull` | 0.8 | | rt_lighting | 2.3 |
| `Present` phase (`renderer->update` and its neighbours) | 6.5 (7.8) | | rt_scene | 1.6 (6.4) |
| `endFrame/post-process` | 5.7 (12.8) | | shadows | 1.3 |
| - `rt scene sync` (WMO/M2 `syncRtScene`) | 3.8 (5.0) | | sky | 0.6 |
| - `rt lighting` recording | 1.8 (8.9) | | volumetric fog | 0.3 |
| `renderWorld` | 5.8 (8.2) | | post-process | 0.1 |
| - `wait for workers` | 5.4 (7.7) | | | |

What this baseline says, before anything is changed:

- At these settings the frame is bound on both sides: the GPU takes 30 ms of
  a 32 ms Dalaran frame, and the CPU, without its 4.5 ms of waiting for it,
  about 27. Taking either alone gains little; the earlier "29 ms CPU against
  12 ms GPU" was measured at other settings and does not hold here.
- `beginFrame` is no longer a black box. Its largest part is recording the
  shadow pass's casters (6.5 ms, 8 in Stormwind), not waiting; the wait for
  the GPU is 4.5 ms on average and 11 at p95.
- The ray traced lighting costs the CPU 5.6 ms a frame in Dalaran (scene
  sync 3.8 + recording 1.8), recorded inside post-processing with MSAA on
  and inside `renderWorld` without it (Orgrimmar's run shows it there).
- On the GPU, terrain and WMO are two thirds of the frame. With 8x MSAA at
  2560x1392 that is fill as much as geometry; an ablation or a run at 1x
  would separate the two.
- The tail: a quarter of Dalaran's frames miss 30 fps. Every route has a
  250-530 ms worst frame during the flight - a stall to look for in the
  per-frame stages (`targets and uploads` and `rt lighting` have shown
  20-70 ms worsts).

The numbers move by a few percent between runs of the same build (an
earlier run of the same routes: Dalaran 31.2 fps, p95 38.2; Stormwind 38.4,
p95 29.8; Orgrimmar 44.0, p95 29.0) - compare changes larger than that, or
run each side twice.

### Dalaran by sample count

The same route with `WOWEE_BENCHMARK_MSAA` (the run's sample count, the saved
setting untouched), commit `64907c4`, 2560x1369, nothing else running. The
last row records the world inline (`WOWEE_SINGLE_THREAD_RECORD=1`).

| MSAA | fps | p50 | p95 | > 33.3 ms | GPU avg | terrain | wmo | m2 | rt (scene+lighting) | wait for GPU (CPU) |
|---|---|---|---|---|---|---|---|---|---|---|
| 8x | 32.9 | 29.2 | 35.1 | 12.6% | 28.7 | 11.1 | 8.2 | 2.2 | 3.8 | 4.3 |
| 4x | 36.9 | 25.9 | 31.6 | 2.1% | 25.7 | 9.1 | 7.1 | 2.2 | 3.9 | 3.3 |
| 1x | 40.4 | 23.1 | 30.2 | 0.8% | 21.8 | 5.6 | 5.8 | 1.9 | 3.5 | 0.1 |
| 1x inline | 36.7 | 25.5 | 32.4 | 2.0% | 21.9 | 5.7 | 6.0 | 1.8 | 3.7 | 0.0 |

Per frame, whatever the sample count: 4900 terrain chunks, 7000 WMO draw
calls, 2660 M2 draw calls. Dalaran has no grass (0.00 ms inline).

- **Terrain is half fill**: 11.1 ms at 8x, 5.6 at 1x. WMO loses 2.4 ms of
  8.2, so it is mostly its 7000 draws and their vertices.
- **At 1x the frame is CPU-bound**: the CPU no longer waits for the GPU at
  all, and the GPU (21.8 ms) waits for a 23 ms CPU frame. Below 8x, sample
  count buys little more; the CPU is next.
- The CPU frame at 1x, per frame:

  | part | ms |
  |---|---|
  | `beginFrame/graph/shadow_pass` - recording the shadow casters | 5.1 |
  | `rendererUpdate/weather and wind/water waves` - `ocean_.advance()` on the CPU | 4.1 |
  | `renderWorld/rt scene sync` - WMO/M2 `syncRtScene` | 3.2 |
  | `beginFrame/graph/reflection_pass` (only without MSAA) | 2.2 |
  | `renderWorld/wait for workers` | 2.1 |
  | `rendererUpdate/M2 animation wait` | 1.7 |
  | `renderWorld/rt lighting` recording | 1.4 (p95 7.2) |
  | `beginFrame/M2 cull` | 0.7 |

  The water waves are the surprise: 4.1 ms every frame in a city with no sea
  in view, from the ocean state the wind's waves added (`61cd56c` and after).
- Recording inline costs 3.7 fps against the parallel workers.

scene_render's static Dalaran scene (`dalaran.json`, 1280x720, hidden window)
is held by its swapchain's FIFO present at 60 fps: `acquire image` takes 8 ms
of each frame and the GPU about 2. It is for timing a renderer change on one
frame, not for the frame rate.
