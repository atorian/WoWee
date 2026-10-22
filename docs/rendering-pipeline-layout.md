# Surface and frame-resource contract

Implementation design for `codex/gpu-scene-pipeline`. This document selects the
target representation. WMO primary-view evaluated surfaces and RT-disabled
lighting are connected behind `WOWEE_WMO_SURFACES=1`; current-frame RT and
the other material families remain pending.

## Surface/material boundary

Geometry evaluates texture combiners, UV animation, POM, texture-derived normals
and coverage once. Lighting consumes those results, the frame's material and
instance tables, and current receiver lighting. It does not repeat texture/POM
sampling. A shared material evaluator serves deferred opaque and ordered forward
layers; the existing WMO shader now calls `shadeWmoSurface` with explicit sampled
surface and lighting material inputs.

| Family | Geometry/coverage | Lighting inputs that must survive | Ordered layers |
|---|---|---|---|
| WMO | Existing UV gradients, POM, normal mapping, lava UV scroll; alpha key 224/255 | Authored vertex RGBA, three distinct normals, WmoLight and transition light, MOHD ambient, SIDN glow/clamp, emissive mode, group shadow participation, local lights | Keep MOBA order; do not move opaque work across a blended dependency |
| M2 | Both texture stages, animated UVs, environment mapping, material opacity, Bayer fade and alpha-to-coverage | Combined albedo, stage-add RGB, foliage height/AO, interior ambient/direct, specular, unlit/emissive, highlight, local lights | Preserve priorityPlane, instance order and multiplicative/additive blend/fog rules |
| Character | Atlas/replaceable textures, combiners, opaque alpha policy, material opacity | Combined albedo, stage-add, interior lighting, specular/unlit, state tint, local lights | Faded parts and modulate layers remain ordered; preserve water boundary |
| Terrain | Layer blending, baked/detail transition, relief normal, LOD seams | Combined albedo, geometric/authored/shading normals, local lights | Distant ground keeps its documented overlap/visibility semantics |

Three normals are intentional. RT receiver identity uses the geometric face;
shadow-map bias uses the authored vertex normal; diffuse/specular use the
normal-mapped shading normal. Substituting one for another changes silhouettes,
shadow bias and thin-surface rejection.

WMO night glow is clamped *inside* each lighting mode before transition mixing
and shadow modulation. A final RGB correction to an already shaded image cannot
represent that operation. Baked MOCV and emissive surfaces are not multiplied by
an outdoor RT shadow indiscriminately. Atmosphere is applied after material
lighting, exactly once at the appropriate layer.

## Selected attachment layout

Three color attachments plus depth, 40 color bytes per sample:

| Attachment | Format | Contents |
|---|---|---|
| Albedo/coverage | R16G16B16A16_SFLOAT, 8 B | Evaluated texture RGB and original alpha; floating attachment zero preserves alpha-to-coverage |
| Surface keys | R32G32B32A32_UINT, 16 B | Geometric, shading and authored normals (each octahedral signed 16+16 bits); material-table index |
| Surface payload | R32G32B32A32_UINT, 16 B | Two packed half pairs: WMO vertex RGBA, or M2/character stage-add RGB plus foliage factor; stable instance token; packed half2 screen motion |
| Depth | Device depth format (budget D32_SFLOAT, 4 B) | Current visibility depth |

Material kind is in the material record; payload meanings are selected by that
explicit kind, not inferred from a model name or overloaded at runtime. Material
and instance records use immutable publication and slot catch-up. Generation
and scene identity are part of the CPU handle/lifetime contract. A compact
surface token must not be recycled while any history or queued snapshot can
name its old owner; wrap invalidates history before token reuse.

Packing introduces quantization. Before activating this layout, fixtures must
measure lit-image error at grazing/specular angles, on transitions and on dark
HDR values. Half overflow/NaN is an explicit invalid surface/history condition.
Motion must be computed from the previous *rendered* transform/deformation and
jitter, never merely the preceding extraction. Newly spawned or replaced
objects carry invalid history explicitly in the instance record.

## MSAA and lifetimes

Do not resolve material IDs, normals or payload with a color average. Lighting
reads the corresponding multisample receiver and shades covered samples before
color resolve. Equal receiver samples may share a validated RT result; uncovered
or different receivers need their own current estimate. Per-sample coverage is
retained. Unsupported sample-count/format combinations are reported explicitly,
not silently changed to 1x.

The opaque G-buffer is transient within one graphics-queue submission. Its
images may be shared across frame slots only with explicit previous-read to
next-write dependencies. CPU snapshots, material/instance uploads, RT history
and outputs retain their own slot/version lifetimes. A slot fence alone is not
a lifetime guarantee for shared transient images. Multiple queues require an
additional semaphore/ownership contract and are outside this graph's API.

Budget at 2560x1369 with 44 B/sample including depth:

| MSAA | One transient set | Two independent slot sets |
|---|---:|---:|
| 1x | 147.06 MiB | 294.12 MiB |
| 2x | 294.12 MiB | 588.24 MiB |
| 4x | 588.24 MiB | 1176.48 MiB |
| 8x | 1176.48 MiB | 2352.97 MiB |

At 3840x2160 a single 8x set is 2784.38 MiB. This cost rules out allocating two
complete G-buffers by default merely because there are two frames in flight.
These totals exclude depth-format padding, RT/history, scene tables and allocator
alignment. Resize should retire or drain the old transient set before allocating
its replacement when their combined peak exceeds the device memory budget.

Capability checks must query sampled/color-attachment support for all formats,
image-format sample counts, depth sampling, max image extent, at least three
color attachments, and storage-buffer ranges. Indirect-count, descriptor
indexing and compute skinning are separate later capabilities, not hidden
requirements of basic G-buffer lighting.

## Current-frame RT and unresolved receivers

Retain the half-resolution base trace. A central resolve verifies the nearby
footprint against the current depth, geometric normal and surface identity.
Unrepresented receivers go into a bounded GPU worklist for additional rays in
the same submission. Capacity covers the worst-case visible receiver count;
overflow must not silently reuse background or historical lighting. No expanding
search in old images is allowed. History reduces noise after valid correspondence;
it is not a substitute for a missing current receiver.

Forward layers need their own receiver evaluation, including surfaces in front
of opaque depth and reflections from another view. Their implementation must be
chosen and measured before the stale forward lookup is removed. Reusing the
opaque pixel behind a transparent surface does not satisfy this contract.

## Remaining gates

- Implement attachments, immutable material/instance tables and geometry output.
- Connect lighting with RT disabled and verify all material/order cases.
- Implement per-sample RT resolve/worklist, rendered-frame motion and history.
- Connect forward and reflected receivers with equivalent material semantics.
- Measure total frame cost, peak memory and replay percentiles before enabling
  the new path as the ordinary renderer.

## First connected stage: WMO, RT disabled

The primary WMO pass uses the selected three color formats and the scene's
existing depth attachment. Geometry and forward shading share texture/POM/
normal evaluation. Opaque/cutout runs store evaluated surfaces; the lighting
pass shades covered samples and applies atmosphere. Before an ordered blended
batch the preceding opaque run is lit into the scene, then the existing forward
batch draws there. Subsequent opaque work begins a new run. Thus lighting is
not necessarily one fullscreen draw per frame; no opaque work moves across a
blended dependency. Reflected WMO stays forward at this stage.

The temporary payload is vertex RGBA (two half pairs), exact raster depth bits,
and a reserved zero word. It contains **no instance token or motion yet**.
Reconstruction uses inverse camera-relative view/projection and adds the camera
position only after dividing by W. Sampled depth is carried in the surface
payload to avoid reading the scene depth while lighting attaches it. Geometry
uses pixel-centre interpolation; under MSAA each covered sample receives that
invocation's payload, and lighting reads samples individually before color resolve.

A frame-slot SSBO holds 80 bytes per submitted opaque material, including the
captured group's local lights. Extraction accumulates an upper bound while
classifying groups. Recording uploads each material while issuing its geometry,
then flushes before submission; it does not build a second draw list or read
live scene objects. This SSBO is not the planned persistent material table.

Three images are shared on the graphics queue with render-pass dependencies
covering prior readers/writers. Upload buffers and descriptor sets are per slot,
updated only after acquisition of its fence. Target generations distinguish
recreated attachments even if the driver reuses handles. Replaced resources
retire behind all frame fences. MSAA scene continuations retain unresolved color
and depth; ordinary forward-only MSAA keeps its previous discard behavior.

At 2560x1369 the new color storage alone is 133.69 MiB at 1x and 534.77 MiB
at 4x; depth is borrowed, not allocated a second time. These are logical image
bytes, not measured allocator overhead. Automatic peak-budget handling on resize
remains pending. The stage rejects RT-on or wireframe explicitly with an error;
it is opt-in and is not the ordinary renderer.

## WMO command recording and ordered execution

Resource preparation runs on the render owner after acquiring the frame slot.
It creates/replaces attachments and descriptors, sizes the material upload and
resets that slot's dedicated command pool. One WMO worker then records from the
immutable snapshot. During the existing traversal it opens a secondary command
buffer whenever the authored sequence switches between evaluated opaque/cutout
and forward blended work. The result is an ordered list of command runs, not a
second copy of model/group/batch data. Pipeline, frame descriptor and surface
settings are bound once per run; geometry and material bindings change only
when required.

After joining workers, the render owner walks only the run list. An opaque run
executes in the surface render pass and is lit before the next blended run.
Blended commands inherit the scene continuation pass. The owner restores the
surrounding pass's inline/secondary recording mode. The serial configuration
uses the same recorder and executor, with recording on the owner. Empty frames
execute no stale plan. A failed WMO recording is propagated after all workers
join; a partially recorded command plan cannot be submitted.

Each frame slot retains its command pool/buffers up to its high-water mark;
reset occurs only after the slot fence. Target recreation does not add a pool
per target. Pool destruction is deferred like other in-flight resources. The
material upload has a single worker writer and is flushed before submission.
The command plan can be discarded on slot reuse; submitted command buffers
retain their own lifetime. No producer-scene reads or additional model walk are
introduced.

## Connected current-frame WMO RT

`WOWEE_WMO_SURFACES=1` now accepts RT modes 1–3. Before the world pass,
the render owner updates the acceleration structure from its already extracted
publication. The legacy post-world RT path skips this update, so there is one
acceleration update and no second producer/model traversal. Other families,
blended WMO and reflected views still consume legacy RT; that work remains until
those consumers are converted. Wireframe remains explicitly unsupported here.

Each opaque run produces current material/depth/geometric-normal receivers.
One representative per 2x2 block traces the shared sun/AO/GI estimator. Compatible
receivers share it; other covered samples enter a worst-case-sized GPU queue
and an indirect dispatch traces them in the same submission. Identical MSAA
receivers are coalesced only when material, depth and geometric normal agree.
Normals face the visible side before tracing. Empty samples are neutral, and
receivers outside RT distance are faded by the material consumer. A central
geometry-aware filter resolves the estimates before lighting reads one result
per sample. Its 8x8 groups cache a 16x16 input tile including the filter halo.

The material upload is now 176 bytes: the original 80-byte material/light data,
a current-to-previous camera-relative transform, a 128-bit lifetime identity,
and history validity. Placement IDs and immutable batch-publication IDs are
monotonic and separate from reusable public model/instance IDs. The WMO worker
captures previous rendered transforms during its existing traversal. History
uses one projected location, validates identity/plane/normal/depth and averages
only after a fresh trace; it does not search old pixels for missing receivers.
Frame gaps, changed RT mode/distance, replaced placements/material publications,
missing previous placement or cloth deformation invalidate correspondence.

Temporal records are 48 bytes per half-resolution block per frame slot; only
the base representative is accumulated. Additional queued receivers currently
have spatial filtering only. When multiple opaque runs cover a block, its last
valid representative owns the saved history. Earlier layers may therefore start
fresh next frame. Rigid transform correspondence is implemented; a dedicated
moving-transport image test is still required before claiming motion parity.

RT scratch uses 20 bytes per full-resolution sample (raw, filtered and queue),
plus a 16-byte indirect command. Sun/AO use unorm8 and signed GI uses three
half-floats. At 2560x1369, scratch plus two history slots adds 147.12 MiB at 1x
or 347.66 MiB at 4x, beyond surface images and material uploads. These are
logical storage sizes, not allocator or measured process-memory peaks. Device
storage-range and descriptor limits are checked; peak resize-budget policy is
still pending. Scratch stays shared on one graphics queue, uniforms/descriptors
and histories are per slot, and replaced resources retire behind frame fences.

Run ordering is preserved: compute happens after that run's geometry and before
its lighting/next blended run. Rays skip uncovered samples, but classification
and filter dispatches still scan the target for each opaque run. Sparse tile
dispatch, persistent material tables and removal of legacy consumers remain
optimization work. The measured total, including the retained legacy RT, is
recorded in `tasks/performance/dalaran-wmo-current-rt.json`. This partial WMO
implementation remains opt-in and does not complete the all-family pipeline.
