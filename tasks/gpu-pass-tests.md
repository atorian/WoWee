# Pass tests: replace the picture checks with tests of the renderer's passes

User decision (2026-11-03): the Python picture checks in
`tools/scene_render/check_*.py` go. Each is replaced by a C++ test that builds
the real pass of `wowee_render`, hands it synthetic input it knows the answer
for, runs it on the GPU and reads the result back as numbers.

Status: plan. Nothing built yet. Decisions D1-D3 below are open.

## Why

- **They say little when they fail.** A failure is "pixels differ by more
  than 48 in 0.1% of the picture". The pixel rectangles are hand-placed
  (`check_fog_height.py:36`: y 195-240, x 600-680), and the thresholds were
  tuned to one machine (`check_cull_turn.py`: `FAR = 48`, `HOLES = 0.001`).
  The fixtures are real places in the world (Dalaran, Hyjal, Goldshire,
  Tanaris) with streamed terrain, 12-90 frames each.
- **They need the game data, so CI never runs them.**
  `.github/workflows/build.yml:169` runs plain `ctest`, never `-C gpu`. Not
  one of the 19 registered checks, nor any `scene_reference_*`, has ever run
  in CI. They run only when someone types `./test.sh --gpu`.
- **8 of the 28 run nowhere.** These are `aerial_march`, `atmosphere`,
  `cloud_fog`, `clouds`, `distant_landmark`, `fog_height`, `fog_sky` and
  `m2_distance_visibility`. Six of them only read PNGs that someone captured
  by hand.
- **Production code carries switches that exist only to draw the checks'
  control pictures.** These are `WOWEE_M2_NO_SKINNING`,
  `WOWEE_M2_NO_BILLBOARD`, `WOWEE_CULL_ON_CPU`, `WOWEE_TERRAIN_FRUSTUM_CULL`,
  `WOWEE_DISTANT_TERRAIN_DELAY_MS`, `WOWEE_FACE_CULLING` and
  `WOWEE_M2_NO_TEXTURE_ANIMATION`. `scene_render/main.cpp` also has a dozen
  scene keys that only the checks use (`mutateAfterExtract`,
  `removeSceneAfterExtract`, `cloudProbe`, `lookAtLast`, ...).
- **They are still being added.** `check_m2_texture_animation.py` arrived
  with GPU-driven stage 3.4d (`092fe8e1`) while this plan was being written.

## What a pass test is

- **It builds the pass class the renderer uses.** It never builds a pipeline
  of its own around a `.spv`. A copy of the pipeline would miss exactly what
  breaks in practice: a binding, a push constant, a barrier, a format
  (AGENTS.md, 2026-10-31).
- **The input is synthetic, and the answer is known without the code under
  test.** Examples: a three-bone `M2Model` whose posed matrices are written
  in the test as literals; a 3x3 grid of terrain chunks and a frustum that
  sees exactly chunks 1, 4 and 7; one white pixel on black for bloom. No game
  data, no `AssetManager`, no DBC.
- **The output is read back from the GPU:** a buffer (bones, particle quads,
  cull verdicts, stats) or a 32x32 offscreen target (colour, depth). The test
  compares it with exact values, or with a tolerance it states and justifies
  (float precision of one operation, not "looks the same").
- **It is a Catch2 case in one suite, `test_render_gpu`.** The suite links
  `wowee_render` alone. Each file is a ctest entry in the `gpu` configuration,
  so the two-lane lock holds as it does today. A machine without Vulkan
  skips (Catch2 exit 4), as the data-reading suites already do.

## What has to be built first (the costs)

The passes cannot be built alone today. Every gap below is a real change to
`wowee_render`, not test scaffolding.

### F1. A Vulkan context without a window

`VkContext::initialize(SDL_Window*)` (`vk_context.hpp:74`) is the only way
to create a device. It needs a surface and a swapchain, and it builds the
main render pass (`getImGuiRenderPass()`) and the overlay pass from the
swapchain format. Every existing GPU test opens a hidden SDL window and
builds the whole `Renderer` on Northrend data.

- Add `VkContext::initializeOffscreen(OffscreenConfig{extent, colorFormat,
  depthFormat, samples})`. It creates the instance without surface
  extensions, the device, VMA, texture table, pipeline cache, command pools
  and queries, and the main, overlay and scene-continue render passes from
  the given formats.
- Split `createImGuiResources` into two parts: creating the render passes
  from formats, shared by both paths (one producer), and the swapchain
  framebuffers, window path only.
- Audit every `getSwapchain*()` caller. Those that only want the format read
  `colorFormat()` instead.
- Have `Bloom` take its source image's layout from the caller.
  `Bloom::record` hard-codes `PRESENT_SRC_KHR` (`bloom.cpp:203-216`), which
  does not exist without the swapchain extension.
- Cost: large. `vk_context.cpp` is about 3,400 lines and assumes a swapchain
  in many places. Risk: the window path must stay byte-identical. That is
  checked with `scene_reference_*` and the client.

### F2. The per-frame descriptor set gets one producer outside `Renderer`

Fog, clouds, starfield, sky, and the M2, terrain and character draws all
take `perFrameLayout` and a per-frame set. That 13-binding layout, its UBO
and its default images are built inline in
`Renderer::createPerFrameResources` (`renderer.cpp:117-423`), and only
`Renderer` can make one.

- Move the layout, UBO, default bindings and the shadow render pass
  (depth-only D32) into a `PerFrameResources` class in `wowee_render`.
  `Renderer` owns one, and a test builds one.
- Cost: medium, about 300 lines moved and the call sites adjusted.

### F3. The culls become passes of their own

- **M2 cull.** The `m2_cull.comp` pipeline, `CullRowGPU` and its buffers are
  private to `M2Renderer` (`m2_renderer.hpp:1147-1236`). Extract an
  `M2CullPass` that owns the pipeline, the row table and verdict collection.
  `M2Renderer` owns one.
- **Terrain cull.** Extract `TerrainCullPass` from `TerrainRenderer` the
  same way. `TerrainRenderer::initialize` refuses a null `AssetManager`
  (`terrain_renderer.cpp:245`), so today nothing of it can be built without
  data.
- Cost: medium each.
- **Ordering.** `M2Renderer` is being reworked by GPU-driven stage 3
  (`m2_draws.comp`, 3.4d now). Extract `M2CullPass` together with that work
  or right after it, never in parallel on the same files.

### F4. Results a test can read

- **Buffers without `TRANSFER_SRC`.** The particle quads, ribbon vertices
  and draws (`m2_particle_pass.cpp:179-202`) and the geometry pages
  (`geometry_pool.cpp:31-35`) are created without it. Add the usage flag; it
  costs nothing at run time.
- **Private images.** Expose the volumetric fog volume image (only its view
  is public today) and the bloom target for reading.
- **Results arrive one record late.** `SkeletonPass`, `M2ParticlePass` and
  the culls deliver what they read back on the next `record()` to the same
  slot. The fixture hides this in a single `runAndCollect()` call.

### F5. The fixture

- **`tests/gpu/render_gpu_fixture.hpp`.** It holds one offscreen
  `VkContext` per process and skips when there is none. Its helpers:
  `submitAndWait(cmd)`, `readBuffer`, `readImage` (RGBA8 / RGBA16F / D32 to
  floats), `upload`, `offscreenTarget(w, h)`, and builders for synthetic
  `M2Model` (bones, tracks, emitters, batches), `TerrainMesh` and 1x1
  textures. `rt_renderer_changes_main.cpp:36-49` already builds a synthetic
  `M2Model`; it moves into the fixture.
- **`wowee_add_gpu_suite_test(test_render_gpu test_x)`.** It works as
  `wowee_add_suite_test` does, but registers through `wowee_add_gpu_test`.

## Each check and what replaces it

Class: A = compute-shader maths, B = what a draw pass writes, C = renderer
bookkeeping (ranges, verdicts across frames, frame snapshots), D = look of a
whole frame.

| Check | Class | Replaced by | Needs |
|---|---|---|---|
| character_skinning | A | `SkeletonPass`: three-bone chain with a keyed rotation; `takeReadBones()` equals literal matrices; a child is composed onto its parent | F1, F5 |
| m2_billboard | A | the same, with a `kTurnBillboards` bone under a rotated model: its basis faces `frame.view` | F1, F5 |
| shared_skeletons | C | `SkeletonPool`: 200 placements of one skeleton use one page; the 201st model is still posed | F1, F5 |
| m2_skinning | C | `M2Renderer` with two skinned synthetic models in one frame: each owner's bones are its own (bone ranges do not overlap or swap) | F1, F2, F5 |
| m2_particles | A | `M2ParticlePass`: an emitter on bone 2 with a known offset; every quad centre lies within the emitter's radius of the posed bone; a ribbon starts at its bone; two runs with one seed are byte-equal | F1, F4, F5 |
| m2_texture_animation | A | `m2_draws.comp` through the M2 draw pass: a batch's UV transform at clock t equals the track's literal value at t | F1-F3 |
| bloom | A/B | `Bloom`: one white pixel on black gives a halo that falls off with distance, leaves far pixels alone, and adds bounded energy | F1, F4 |
| fog_height, aerial_march, atmosphere | A | `VolumetricFog` with a uniform medium: transmittance along the slices is `exp(-σd)` within float precision; height influence 0 -> 1 thins the fog above; strength 0 equals no fog; weather fog is integrated once; Low and High quality agree within a stated bound | F1, F2, F4 |
| terrain_cull | A | `TerrainCullPass`: a 3x3 grid and a frustum give exactly the chunks the test names; a seam between LOD levels gets its skirt | F1, F3 |
| cull_turn | C | `M2CullPass` + `cull_guard`: a view the guard does not cover is culled on the CPU; a nudge inside the guard keeps what came into view | F1, F3 |
| face_culling | B | M2 and WMO draw into 32x32: a one-sided material facing away draws nothing; two-sided (M2 0x04, WMO `F_UNCULLED`) draws; a mirrored reflection matrix still culls the back | F1, F2 |
| m2_foliage_shadow | B | M2 shadow pass into a 32x32 D32 target: a quad with a checkerboard alpha writes depth only where alpha >= alphaRef | F1, F2 |
| display_skins | C/B | `CharacterRenderer` (already takes a render pass override): two instances of one model with red and blue skins draw red and blue | F1, F2 |
| shared_geometry (m2, character) | C | two models in one `GeometryPool` page: each draw covers its own triangle's pixels and no other's | F1, F2, F4 |
| m2_moved | C | M2 draw: an instance moved after extraction is drawn, and casts its shadow, only at the new place in the kept frame | F1, F2 |
| frame_inputs (13 entries) | C | per renderer (m2, wmo, character, terrain, water): extract, mutate or remove the source, record; the target equals the unmutated one. The post-process, RT, FSR2, MSAA and rain modes go through `Renderer` on the offscreen context with a synthetic scene (open question Q1) | F1, F2 |
| interface_frame | C | `InterfaceRecorder` (already standalone): mutating, growing or destroying the source after extraction does not change the target | F1 |
| rt_radius | C | `RtScene` region selection: two instances, one 4,000 yards away, radius 100: one resident instance | F1 |
| streamed_world_settled | C | `DistantTerrain` with a loader the test completes on demand: the frame is the same before and after the load lands. `WOWEE_DISTANT_TERRAIN_DELAY_MS` goes | F1, F2 |
| distant_landmark | C | `M2Renderer` with more instances than one instance page: all are recorded, or the overflow is logged | F1, F2 |
| m2_distance_visibility | B | M2 draw at the edge of the retirement range: coverage of a solid quad is complete (no dither holes); the fade stays inside the cull range | F1, F2 |
| sharp_stars | B/C | `StarField` draws a star at its projected pixel; with sharp stars on, the sky model's star batches are not drawn (draw count) | F1, F2 |
| clouds, cloud_fog | B/D | `Clouds`: density 0 leaves the sky byte-equal; overcast covers above a stated share; an opaque quad's fogged colour does not change with the cloud phase. The rest of `check_clouds.py` is visual review (D3) | F1, F2 |
| fog_sky | D | D3 | |

## Order

1. **Decisions D1-D3 and the AGENTS.md text.** No new `check_*.py` from then
   on. Cheap, and it stops the inflow first.
2. **F1 + F5, then the first test group**, which needs neither F2 nor F3:
   `SkeletonPass` (character_skinning, m2_billboard), `SkeletonPool`
   (shared_skeletons), `M2ParticlePass` (m2_particles, with F4),
   `InterfaceRecorder` (interface_frame), `RtScene` (rt_radius) and `Bloom`.
   Delete the replaced checks, their reference PNGs, their CMake entries and
   the switches only they used, in the same commits.
3. **F2**, then fog, clouds, starfield, face culling, foliage shadow, display
   skins, shared geometry, m2_moved and per-renderer frame snapshots.
4. **F3 for terrain**, then terrain_cull.
5. **F3 for M2**, after GPU-driven stage 3 has settled `M2Renderer`, then
   cull_turn, m2_skinning, m2_texture_animation, distant_landmark and
   m2_distance_visibility.
6. **What remains.** The `Renderer`-wide frame snapshot modes (Q1), then
   delete what nothing uses any more:
   - `rgb_pixels` and the rest of `tools/scene_render/check_*`
   - `clouds.ps1`
   - the check-only scene keys in `scene_render/main.cpp`
   - the 8 unregistered scripts and their saved `*-results.json`
   - `tools/scene_render/reference/`

Work in a worktree branch (subsystem change), one group per commit. Each new
test is first run against the bug it guards: break the code, see it fail,
restore it (AGENTS.md).

## Decisions for the user

- **D1. AGENTS.md.** "What runs on the GPU is tested on the GPU" stays. Its
  method changes from "a scene drawn by `scene_render` and the picture
  checked (`check_*.py`)" to "the pass built alone, synthetic input,
  results read back". Whole-frame approvals (`scene_reference_*`) stay as
  the only acceptance of a whole frame.
- **D2. CI.** Without game data, the suite can run in CI on Mesa's software
  Vulkan (lavapipe; `mesa-vulkan-drivers` on the Ubuntu job). It executes
  the real SPIR-V through a real Vulkan driver, with real barriers and
  formats. Precision and dispatch order still differ from a vendor GPU, so
  the local `./test.sh --gpu` run on the real GPU stays. Options: run in CI
  on lavapipe, or keep GPU tests local only, as today.
- **D3. Class D, the look of a whole frame.** This covers `fog_sky` and the
  visual-review half of `check_clouds.py` (shape, softness, gaps). No pass
  test can say "the clouds look right". Options:
  - (a) a `scene_reference_*` approval scene each;
  - (b) `scene_render` stays a viewing tool, with no test;
  - (c) drop them.

## Open questions

- **Q1.** Can `Renderer` be initialised on the offscreen context with a
  synthetic scene and no `AssetManager` or map? This is needed for the
  frame snapshot modes (post-process, RT 1-3, FSR2, MSAA+FXAA, rain).
  `initializeRenderers(&assets, map)` suggests not. If not, those cases wait
  for whoever makes `Renderer` buildable without data, or are tested per
  pass (`PostProcessPipeline`, `RtLighting`) instead.
- **Q2.** `M2Renderer::initialize` accepts a null `AssetManager`, but a
  batch that names a texture dereferences it (`m2_renderer_instance.cpp:872`).
  Synthetic models must use the texture table directly, or texture loading
  must go through an interface the fixture can satisfy.
- **Q3.** The 6 `wowee_add_scene_test` entries that match `scene_render`
  stdout with a regex (transport hidden/shown/docked, m2_counts) are the
  same kind of test. Are they in scope? They are not Python.

## Not in scope

`glsl_as_cpp.hpp` runs shader maths on the CPU for gameplay (ocean height,
sea bed, shelter). Its tests (`test_ocean_spectrum`, `test_sea_bed`,
`test_shelter_probe`) test that CPU copy, which is a separate question (see
`tasks/test-review/batch-1.md`).
