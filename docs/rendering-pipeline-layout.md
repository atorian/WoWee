# Surface and frame-resource contract

Implementation design for `codex/gpu-scene-pipeline`. This document selects the
next implementation's representation; it does **not** claim these attachments
or current-frame deferred lighting are already connected to Renderer.

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
