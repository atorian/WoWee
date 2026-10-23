# WoW 3.3.5a Client Render Specification

What the original client (WoW.exe, build 12340, image base 0x400000) does when
it draws the world, recovered by static disassembly. The renderer should match
this behaviour; where it differs on purpose, say so next to the code.

Every finding carries the function address it comes from and a confidence:

- **high** - read directly from the code path;
- **medium** - code read, but a field's meaning or a caller is inferred;
- **low** - plausible reading, not confirmed.

Method: capstone disassembly of the binary, never run. The tooling lives in the
worktree's `build-dev/scratch/particles_re` (`wowre.py`, `all.asm`, call graph
`q.py`). Engine conventions used throughout:

- `g_gxDevice` is `[0xc5df88]`. `0x685970` = `GxRsSet(state, value)`,
  `0x685f50` = set texture state, `0x409670`/`0x685fb0` = push/pop render state,
  `0x681f60` = viewport `(x0, y0, x1, y1, minZ, maxZ)`, `0x6813b0` = clear.
- `EGxRenderState`: 0 PolygonOffset, 1 MatDiffuse, 2 MatEmissive, 3 MatSpecular,
  4 MatSpecularExp, 5 NormalizeNormals, 6 BlendingMode, 7 AlphaRef, 8 FogStart,
  9 FogEnd, 10 FogColor, 11 Lighting, 12 Fog, 13 DepthTest, 14 DepthFunc,
  15 DepthWrite, 16 ColorWrite, 17 Culling, 18 ClipPlaneMask, 19 Multisample,
  20 ScissorTest, 21 Texture0, 22 Texture1, ... (confirmed against the state
  shadow array at `[[0xc5df88]+0x28f4] + state*0x18`).
- `EGxBlend`: 0 Opaque, 1 AlphaKey, 2 Alpha, 3 Add, 4 Mod, 5 Mod2x, 6 ModAdd,
  7 InvSrcAlphaAdd, 8 InvSrcAlphaOpaque, 9 SrcAlphaOpaque, 10 NoAlphaAdd,
  11 ConstantAlpha, 12 Screen, 13 BlendAdd.

---

## 1. Frame order

### 1.1 `CWorldFrame::RenderWorld` (0x4f8ea0) - high

1. Push render state, clear to black when needed, set the world viewport with
   depth range **[0, 0.94]** (`0xadeee4` = 0.94).
2. `CWorldScene::Render` (0x77eff0 -> 0x79a870): culling, terrain, WMO, sky,
   and the M2 scene's per-frame animate/collect (section 1.2).
3. Footprints (0x77f070 -> 0x79fcc0).
4. **M2 pass 0** - opaque (`CM2Scene::Draw(0)`, 0x823cb0).
4a. Projected decals (0x4f8a40, `projectedTextures`), then 0x77f010 -> 0x7984a0.
5. Then, depending on whether the camera is under water
   (`[0xcd8794]`, read by 0x780620):

   | Camera above water | Camera under water |
   |---|---|
   | M2 pass 2 (blended, below the water plane) | 0x77f980 (barrier effects) |
   | liquids (0x77f020 -> 0x790a80 -> 0x8a2240) | M2 pass 1 (blended, above water) |
   | weather (0x77f030 -> 0x78ca50) | weather |
   | 0x77f980 (barrier effects, `CBarrier`) | liquids |
   | M2 pass 1 (blended, above water) | M2 pass 2 (blended, below water) |

   That is back to front relative to the water plane: whatever is on the far
   side of the water surface from the camera goes first.
6. Spell/unit effects and overlays (0x7fca30, 0x7f9ec0, 0x6fdfb0, 0x4f6f90),
   underwater screen effect when under water (0x77f9d0 -> 0x79ca70),
   0x7f0870, nameplates (0x7e5580).

Characters are not a separate pass: units are M2 models in the same M2 scene
and their batches are sorted together with doodads (section 3).

### 1.2 `CWorldScene::Render` (0x79a870) - high for order, medium for some stage names

1. Frame setup (0x790920), frustum (0x984240), per-frame resets, terrain/WMO/
   detail doodad buffer prep (0x782f20, 0x7ba600, 0x7ae060, 0x7b2a80).
2. Culling: camera inside a WMO interior (`[0xcd87a4]` set) runs portal culling
   (0x7b3b20, 0x794190, 0x79a790 with the portal bounds at 0xadf58c, or
   0x794250) and collects the screen rect through which the exterior is
   visible (bbox at 0xadf570..0xadf5a4); outdoors runs 0x79a790 over everything.
   Then 0x79a260, 0x793450 (sets visible models animating), liquid prep.
3. Clear colour: black, fog colour, or the underwater colour (0x6813b0).
4. Player shadow plane (0x7bb670), **M2 scene animate + collect** (0x821a20,
   see 3.1).
5. 0x6fda20 (units/missiles), **shadow-map render** (0x7bb570, section 5.2),
   0x872c10, 0x781610.
6. **Terrain** (0x798da0; Terrain2/Terrain3 shaders).
7. **WMO groups** (0x7964a0 over the visible-group list 0xcdb080, per group
   0x7abf50), then 0x795f80 (WMO portal/antiportal work).
8. If any exterior is visible (`[0xadf580] >= 0`): exterior lists 0xcdd0e8 /
   0xcdd0f8 (0x796c10), 0x7f31c0, and the **sky** (0x7f09b0) unless under water.
9. **Unit blob shadows** (0x793980, section 5.1), 0x8a2f00, liquid instances
   (0x793d20).

### 1.3 Sky (0x7f09b0) - high

- Drawn after terrain and WMO, before the M2 opaque pass.
- Viewport depth range **[0.999023, 1.0]** (`0xadeef0`), so the sky sits behind
  everything the world draws in [0, 0.94] and only shows where nothing is.
- Scissor test on (state 20) to the screen rect where the exterior is visible
  through portals.
- Draws the sky dome/stars (0x9abd50, 0x9ac660 x3, 0x9acb00, 0x9acd40) and the
  skybox M2 through its own M2 scene passes (0x7f08c0 -> 0x823cb0).

### 1.4 WMO batches (0x7ac6a0, default render path) - high

- Render path chosen at 0x7ad047 from `[0xadfe44]` (0..5, default 5):
  0x7aa2e0, 0x7aa690, 0x7aaa40, 0x7aae00, 0x7ab1e0, 0x7ac6a0 (+0x7ac9f0).
- Batches are drawn **in MOBA file order**, opaque and transparent together,
  no sorting.
- Per batch, from its MOMT material: `Culling = !(flags & 0x4)`,
  `BlendingMode = MOMT.blendMode`, texture clamp from flags 0x40/0x80
  (`0x681450`), Texture0/Texture1.
- **Depth write is not changed per batch** - blended WMO batches write depth.
  That is why the sky can follow them without painting over windows.

---

## 2. Culling

All high confidence unless marked.

### 2.1 Frustum machinery

- Frustum = 0xfc bytes (6 planes, 8 corners, eye), kept on a stack at
  `0xcdb168 + depth*0xfc` (depth `[0xcd8798]`; push 0x791950, pop 0x78fb50).
  Camera corners at 0xcdb108 (0x795400); 0x984240 turns corners into planes.
- Tests: AABB p-vertex 0x9839e0 (epsilon 0.0194), sphere 0x983d20, AABB
  fully inside 0x983a60.
- **Every portal-narrowed frustum is built from a screen rectangle**
  (0x790af0 / 0x790e20 lerp the 8 corners) - no per-edge portal planes.
- The test always uses the top of the stack: the full camera frustum outdoors,
  the union rect of outside-leading portals when looking out from inside.

### 2.2 Main pass: 64 distance buckets, near to far (0x79a790)

- Objects are put into **64 buckets of 33.33 yards** along the camera forward
  axis (0xcd9048, 0x6c bytes each): index =
  `dot(nearest AABB point, forward plane) * 0.03 - 0.5`; bucket >= 64 dropped.
- Producers from the map walk (0x7b6b00): terrain chunks 0x792d80, chunk
  liquids 0x792df0, WMO groups 0x792ad0, entities 0x792e60, occluder edges
  0x7927e0.
- For each bucket, near to far: chunks (0x799d40), WMO groups (0x79a160),
  liquids (0x7935a0), entities (0x793060), WMO doodads deferred into it
  (0x7987a0), then this bucket's occluders are written into the horizon
  buffer (0x793760). WDL after the loop (0x791980).

### 2.3 Per object type

| Object | Tests |
|---|---|
| terrain chunk | quadtree to a farclip rect (0x7b4df0); AABB (+0x4c) vs frustum; occluder volumes (sphere); horizon (AABB) |
| chunk doodad (MCRF, 0x799980) | size-class gate (2.6); sphere vs frustum; horizon; no occluder test; dedup per frame |
| WMO doodad, exterior group | re-bucketed by its own distance (0x7998a0); sphere vs frustum, occluder volumes, horizon (0x7987a0) |
| WMO doodad, interior group | sphere vs each `CPortalView` frustum the group was seen through; passes if any does (0x799b70) |
| WMO group seen from outside | only groups with MOGP & 0x10008 (exterior / always-draw) are bucketed; AABB vs frustum, occluder, horizon, then portal traversal into interiors (0x7b3a10 -> `CMapObj::CullFromOutside` 0x7ad350) |
| entity (unit, player, game object) | sphere vs frustum, occluder volumes, horizon (0x793060) |
| liquid | `CChunkLiquid` AABB vs frustum, horizon, occluder; WMO liquid follows its group (MOGP 0x1000) |
| detail doodads | no own frustum test; created only for visible chunks within `groundEffectDist` |

Interior groups are never drawn by their own bounding box from outside - only
when reached through portals of a visible exterior group. Always-draw groups
(0x10000) go straight to the visible list.

### 2.4 WMO portal culling (camera inside)

- Each frame (0x795d40, point-in-group 0x7d59b0): `[0xcd87a4]` = the WMO
  instance the camera is in, its start group(s) in 0xcdb0d4; `[0xcd87a0]` a
  second containing WMO (medium).
- `CMapObj::CullFromInside` (0x7ad1f0) seeds the camera group(s) with rect
  (-1,-1,1,1) and recurses (0x7ac060), **depth limit 10** (`[0xd1bee4]`):
  1. The reached group becomes visible (0x799310 -> list 0xcdb080) and keeps
     a `CPortalView` with the current frustum (for its doodads). No further
     bounds test on the group.
  2. For each MOPR portal of the group (skipping the one we came through):
     - portal polygon clipped, NDC bounding rect (0x7a9090, cached per frame);
       straddling the near plane -> full screen; fully clipped -> empty;
     - **side test**: `dot(portal plane, eye) + d`, sign by `MOPR.side`;
       <= 0 -> not traversed;
     - rect intersected with the incoming rect; rejected if empty or narrower
       than 0.001;
     - if the other group is exterior (flags & 0x50148): record an outside
       view - portal rect into 0xcdd0e8 and unioned into the sky rect
       0xadf570; if 0x10008 also into the exterior rect 0xadf58c; **no
       recursion** into that group;
     - otherwise push a frustum from the intersected rect, recurse, pop.
  3. A group with MOGP 0x40000 selects the WMO skybox (`[0xcd861c]`).
- Afterwards, if any exterior portal was seen (`[0xadf59c] >= 0`, max portal
  distance), the **whole outdoor pass** runs once with the frustum narrowed
  to the union rect of those portals (not per portal); otherwise nothing
  outdoors is drawn.

### 2.5 Occlusion

- **Occluder volumes (`CWorldOccluder`)**: hard-coded in the exe, not map data
  - 62 polygons at 0xaf0040 for maps 0 (a Stormwind wall), 571 (a Dalaran
  disc), 600, 603, 609, 631, 575. Visible ones become shadow volumes from the
  eye (0x7cd4e0); `0x7cce00(sphere)` culls what is fully inside one.
  `CWorldAntiOccluder` appears unused (medium).
- **Software horizon buffer** ("Terrain culling", on by default, `showCull`):
  384 columns of max view-space height (0xcd8938), reset each frame, filled
  front to back by terrain chunk far edges, by WMO groups named
  **"antiportal"** (converted at load into edge segments and never rendered),
  and by occluder polygon edges. Spheres (0x78fc40) and AABBs (0x78fdc0) under
  the horizon in every covered column are culled. Exempt: objects nearer than
  50 yards; the whole test is skipped when `|camera forward.z| >= 0.9`.
- **Hardware occlusion queries** (CVar `occlusion`): used only for sun/moon
  glare visibility (0x9abe00, one frame delay), never for geometry.

### 2.6 Distance and fading

- `farclip`: default 350, min 183.33, max 791.67 (1583.33 with
  `farClipOverride` on old maps, or > 1 GB RAM on others). `nearclip` 0.2.
- WDL low detail beyond: own projection, near = farclip - 50, far =
  `horizonFarclipScale` (default 4, 3..6) x farclip.
- **Doodad size class** (0x7bdb10): max AABB extent vs {1, 4, 15, 100} -> class
  0..4. With s = `environmentDetail` (default 1, 0.5..1.5):

  | Class | max extent | Max distance | Fade length |
  |---|---|---|---|
  | 0 | < 1 | 30 s | 5 |
  | 1 | < 4 | 100 s | 10 |
  | 2 | < 15 | 200 s | 15 |
  | 3 | < 100 | 750 s | 20 |
  | 4 | >= 100 | 1250 | 50 |

- **objectFade** (default on): doodads (without flag 0x800) fade
  `alpha = 1 - (d - (max - fade)) / fade` over the last `fade` yards; < 0.01
  not drawn, >= 0.99 treated as 1. `objectFadeZFill` off (default) leaves the
  z-fill pre-pass only on classes >= 3 (medium).
- Detail doodads within `groundEffectDist` (default 70, max 140).
- No distance LOD for WMO groups beyond buckets and farclip.

### 2.7 What culling does to models

- The M2 animate list is rebuilt every frame from what passed culling
  (`SetAnimating`, 0x823f10); culled models get no bone or particle update.
- Culled **units/entities** keep animating within 33.3 yards
  (`dist^2 < 1111.1`) or with entity flag 0x400 (0x793450).
- Culled **doodads** keep animating within 10 yards or with flag 0x400;
  doodads rejected by the size-class gate are not touched at all.

---

## 3. M2 models

### 3.1 Scene lists and passes (collected in `CM2Scene::Animate`, 0x821a20) - high

Scene `[0xcd754c]`. Draw entries (0x44 bytes) at +0x34; pass lists of entry
indices at +0x54 (pass 0), +0x64 (pass 1), +0x74 (pass 2); BatchDoodad
candidates at +0x44. Rebuilt every frame.

Entry types: 0 Batch, 1 BatchProj, 2 BatchDoodad, 3 Ribbon, 4 Particle,
5 Callback (dispatch table 0x823c90).

Batch skipped when `shader_id == 0x8000`, it has no texture, or its alpha
(model alpha x colour alpha x texture weight) <= 0.0001.

**Opaque vs blended**: decided by the batch's base layer
(`materials[materialIndex - materialLayer]`): blended when its blend mode > 1
or the alpha < 0.99999.

| Entry | Pass |
|---|---|
| opaque batch | 0 (or the BatchDoodad list when batchable) |
| blended batch | 1 when the model is above the water plane, 2 when below, both when it straddles (then clipped, see below) |
| ribbon | 0 if blend <= 1 and alpha >= 0.99999, else 1 or 2 (never both) |
| particle emitter | 0 if emitter blend <= 1 and alpha >= 0.99999, else 1, or 2 if the model is below water or emitter flag `+0x134 & 0x40000` |
| callback | 0 if model flag 0x20, else 1 or 2 |

Above/below water: the model's lighting info plane (`+0xc4..+0xd0`) against
its bounding sphere ("above" = dist >= -r, "below" = dist <= r). The plane and
flags come from the world-object liquid query (0x7c23f0 -> 0x7c10c0): no liquid
-> above; liquid over the object -> below; straddling -> both, plane
(0, 0, 1, -waterHeight). Models with no such query default to above.

A batch drawn in both passes gets the water plane as clip plane 0
(0x81fd5e, ClipPlaneMask): pass 1 keeps the part above water, pass 2 (plane
negated) the part below.

**Z-fill** (M2UseZFill, model flag 0x40, not BatchProj, material without flag
0x10): a depth-only copy of the blended entry (ColorWrite 0, Opaque/AlphaKey)
is drawn first in its pass.

### 3.2 Sorting - high

- **Pass 0** (comparator 0x81eea0): not by distance. By type
  (Batch < Proj < Doodad < Ribbon < Particle < Callback), then state:
  materialLayer ascending (base layers before overlays), shaders, model,
  texture count, blend, material flags, textures.
- **Passes 1 and 2** (0x81ef30): **back to front** by `+0x10` (squared
  view-space distance), z-fill copies first, priorityPlane ascending, then
  `+0x14` descending, model, type, materialLayer ascending.
- Batch distance: the skin section's sortCenter transformed by its
  centerBoneIndex bone, squared view-space distance; M2Batch flags 1/2 move the
  point sortRadius towards/away from the camera (medium on sign). So batches of
  different models interleave by their own distance.
- Particle entries: `+0x10` = model distance, `+0x14` = emitter distance.
  Ribbons and callbacks use the model distance.
- Model distance: squared length of the model's view-space translation; an
  attached model uses its parent's.

### 3.3 Render state per batch (0x81fe90, 0x81fb10) - high

Blend table `0xa453b0[pass*7 + m2Blend]`:

| M2 blend | pass 0 | passes 1, 2 |
|---|---|---|
| 0 Opaque | Opaque | Alpha |
| 1 AlphaKey | AlphaKey | Alpha |
| 2 Alpha | Alpha | Alpha |
| 3 NoAlphaAdd | NoAlphaAdd | NoAlphaAdd |
| 4 Add | Add | Add |
| 5 Mod | Mod | Mod |
| 6 Mod2x | Mod2x | Mod2x |

- AlphaRef: Opaque 0; AlphaKey `224/255 x entry alpha` (the threshold drops as
  the batch fades); every other mode 1/255.
- Material flags: 0x1 unlit (also forced for Mod/Mod2x via 0xa45374),
  0x2 unfogged, 0x4 two-sided (Culling 0), 0x8 no depth test,
  0x10 no depth write.
- **Blended batches keep depth write on**; only material flag 0x10 turns it
  off.
- Fog per blend (0xa45390): Opaque/AlphaKey/Alpha normal fog; NoAlphaAdd/Add
  fog to black; Mod fog to white; Mod2x fog to grey 0.5 (colour mapping medium).
- Draw(pass) resets DepthFunc, PolygonOffset, Lighting, Fog, MatSpecularExp.
- Particles use a synthetic material: two-sided, unlit unless emitter
  `+0xd4 & 1`, unfogged unless `& 2`, **no depth write** unless `& 4`.

### 3.4 Batching - high

- **BatchDoodad** (M2BatchDoodads, 0x824550, 0x820ae0): identical opaque static
  batches of several doodad instances drawn as one instanced call; a group of
  one goes back to pass 0 as a normal batch.
- **BatchProj** (0x820720): batches with `M2Batch.flags & 4` are drawn, then a
  world callback (0x77f500 -> 0x7e3aa0) projects world textures onto them.
- **ParticleBatch** (M2BatchParticles, 0x81f9e0): in passes 1/2, consecutive
  additive emitters (GxBlend 3 or 10) are regrouped and merged into one draw;
  non-additive entries keep their distance order.
  M2ForceAdditiveParticleSort treats every particle as additive for that
  grouping.
- Option bits `[0xd3fcf4]`: 1 M2UseZFill, 2 M2UseClipPlanes, 4 M2UseThreads,
  0x20 M2BatchDoodads, 0x80 M2BatchParticles, 0x100 M2ForceAdditiveParticleSort.

### 3.5 Attached models, animation clock and model flags - high unless marked

- **Attach** (`CM2Model::AttachToParent` 0x831630): child +0x48 = parent,
  +0x50 attachment id, +0x54 attachment index (via the parent's lookup),
  flags |= 0x20080, linked into the parent's child list. With a fixed world
  position it keeps a local offset (+0xe4) and flags 0x8000/0x40000.
- **Every frame** after the parent animates (0x82e550), for each child: the
  attachment's visibility track (M2Attachment +0x14, 0x82b270) - 0 means the
  child is not animated; `M = T(attachment.pos) * parent.bones[attachment.bone]`;
  `CM2Model::Animate(child, M, ...)` (0x82f0f0) with
  `child world = child.local (+0xb4) * M`. Parent bones already include bone
  animation, the parent's world matrix (position, facing, scale) and the view
  matrix, so **children follow bone rotation, facing and scale every frame**.
- Alpha inheritance: `child.+0x19c = child.+0x17c * parent.+0x19c *
  child.+0x178` (0x82f27f) - attached effects fade with the unit (stealth,
  ghost).
- **Track clock** (resolver 0x8284d0): a track with a global sequence uses
  the per-instance `(sceneTime - model+0x74) mod duration`; otherwise the
  instance's current sequence time, lerped with the previous sequence while
  blending. Colour alpha (fixed16 / 32767) and texture weights are evaluated
  per instance per frame in 0x82f0f0, default 1.
- **Bone matrix** (CM2Model::Animate 0x82f0f0, bone stride 0x58: flags +4,
  parent +8, tracks +0x10/+0x24/+0x38, pivot p +0x4c): for an animated bone
  (flags & 0x280) `L = R(q)`, rows scaled, (0x80: x instance matrix),
  `row3 += p + t`, `L = T(-p) L`, `B = L x Parent` (row vectors); otherwise
  `B = Parent`. The root parent is `model.local x view` (or
  `T(attach.pos) x parentBone` when attached), so **bones are in view
  space**.
- **Billboarding** (dispatch 0x82ff3a on `flags & 0x78`): save the row
  lengths `s` and the pivot `P' = p B`; rebuild the 3x3 against the **fixed
  view axis** (screen-aligned, the eye-to-pivot vector is never used); then
  rescale rows by `s`, `B.row3 = P' - p B3x3`. `n()` normalises only when
  |v|^2 > 2.384e-7 (otherwise leaves the vector, no fallback):

  | Flag | Rows |
  |---|---|
  | 0x08 spherical | not animated: r0 = (0,0,-1), r1 = (1,0,0), r2 = (0,1,0); animated (0x200/0x80): `r_i = n(L_i1, L_i2, -L_i0)` from the bone's local matrix - all parent rotation (facing, camera) discarded |
  | 0x10 lock X | r0 = n(r0); r1 = n(r0.y, -r0.x, 0); r2 = r1 x r0 |
  | 0x20 lock Y | r1 = n(r1); r0 = n(-r1.y, r1.x, 0); r2 = r1 x r0 |
  | 0x40 lock Z | r2 = n(r2); r1 = n(r2.y, -r2.x, 0); r0 = r2 x r1 |

  A kept axis parallel to view Z collapses the other two rows to zero (card
  invisible). 0x200 is "has animation tracks", not a billboard bit
  (0x240 = animated + lock Z). Two billboard bits together skip the rebuild
  and only rescale (row lengths squared - a quirk). Child bones inherit the
  billboarded matrix.
- **Model flag 0x2000** is frustum culling, not "particles only": set by
  0x791240 (from the unit blob/visibility loop 0x793980) when the model's
  bounding box fails the current frustum; such a model draws only doodad and
  particle entries (they may reach beyond the box). Cleared by the object's
  force-draw bit.
- Particle-only drawing after an effect ends: `CEffect::Finish` (0x6f87c0)
  clears the draw flag, sets the particle-only flag (0x10000 root / 0x20000
  attached) and stops emitters (flag 2); the model is queued on
  `scene+0x30` while it still has live particles, then deleted (0x6fa450).

---

## 4. Particles (`CParticleEmitter2`, ParticleSystem2.cpp)

### 4.1 Pool - high

- Per emitter, grow-only:
  `pool = (int)((rate + rateVary) * (life + lifeVary) * 1.15)` from the current
  animated values (0x97edf0, reserve 0x97e480), re-evaluated every frame the
  emitter emits. No fixed per-emitter or per-model cap.
- Child emitters: `min(childRate * childLife * 1.15 * parentPool, 4096)`.
- Spawning takes a slot from a free list; when it is empty spawning stops (no
  recycling) and the remainder stays in the accumulator.

### 4.2 Emission (0x97d8c0) - high (distance meaning medium)

- `rate = (emissionRate + rateVary * U[-1,1]) * particleDensity * distLod`,
  the random term drawn once per frame.
- `distLod = clamp(1 - (d - 50) * 0.02, 0.25, 1)`, d = emitter to camera
  distance: full rate to 50 yards, 25% from 87.5 yards. Runtime emitter flag
  0x400000 (set by game code for particular objects) skips it.
- `accum += rate * dt; n = round(accum)`, spawned while slots remain,
  `accum -= spawned`. No other per-frame clamp. New particles start at
  `age = rand01 * dt` and are aged by dt in the same update.
- Particle life: `max(0.001, life + lifeVary * seed/32767)` - but the plane,
  sphere and spline spawners never write the seed, so **lifespanVary is
  effectively not applied to M2 particles** (high/medium, see 4.5).
- `particleDensity` CVar: float 0.1..1.0, default 1.0 (0x78d860 -> 0xb2d678);
  scales emission only, never the pool. No `particleMTDensity` in 3.3.5a.

### 4.3 Update (0x97acb0, 0x97dd20) - high

- `dt > 0.1 s` is simulated in 0.1 s substeps, at most one lifespan's worth.
- Culled models are not animated (section 2), so their particles freeze.

### 4.4 Draw - high

- Per emitter at most `0x4000 / vertsPerParticle` particles drawn: 4096 for
  head-only, 2048 for head + tail; the rest are simulated but not drawn.
- M2BatchParticles merges emitters while a draw stays within 1 MB of vertex
  data and 0x20000 indices; shared static uint16 quad index buffer of 0x1fff8
  indices ("CParticleEmitter2_idx").
- No global per-frame particle budget exists.

### 4.5 Simulation details - high unless marked

Notation: E+x emitter field, R+x M2Particle record field (stride 0x1dc),
randS in [-1, 1], rand01 in [0, 1), from the emitter RNG at E+0x24.

**Creation** (CM2Model 0x832ea0, loop 0x833b40..0x83413c). Emitter type
R+0x29: 1 plane (0x981310, spawn 0x9815c0), 2 sphere (0x9813f0, spawn
0x981950), 3 spline (0x9820f0, spawn 0x981d40); nothing else. FBlocks are
kept as pointers: colour R+0x104, alpha R+0x114, scale R+0x124, head cell
R+0x13c, tail cell R+0x14c. Scalars: tailLength, twinkle speed/percent/scale
(min, max-min), burstMultiplier, drag, base spin/vary, spin/vary, tumble,
wind + windTime, follow speed/scale (as slope/intercept). R+0x2c/0x2d
(particleType, headOrTail bytes) are **not read**; textureTileRotation R+0x2e
appears unused (medium). Gravity is a plain scalar in this build.

**M2 particle flags (R+0x04)**:

| Flag | Effect |
|---|---|
| 0x1 | **unlit** (vertex format without normals; Particle_Unlit) |
| 0x2 | sort particles back to front by view depth |
| 0x4 | head quad aligned to the projected velocity |
| 0x8 | unfogged |
| 0x10 | model/local space: spawn not transformed, drawn with the emitter world matrix |
| 0x20 | size x bone world scale (otherwise model scale does not change billboard size) |
| 0x40 | inherit emitter velocity: every >= 1/30 s `v_e = (pos - prevPos)/dt * burstMultiplier`; at spawn `vel += v_e (1 + randS speedVar)` |
| 0x80 (sphere) | kill the particle once it passes the centre |
| 0x100 (sphere) | velocity along +Z instead of radial |
| 0x200 | random spin direction (every other slot negated; model particles random tumble sign) |
| 0x400 | tail length = min(tailLength, age) |
| 0x800 | spawn position lerped by rand01 between previous and current emitter position |
| 0x1000 | XY quad: in the emitter's XY plane, not camera facing |
| 0x2000 | at spawn `pos.z = groundHeight + max(scale.x, scale.y at birth)` |
| 0x4000 | follow: particles older than 2 dt move with the emitter delta x clamp(intercept + slope |delta|/dt, 0, 1) |
| 0x8000 | burst: on the rising edge of "enabled", spawn floor(rate) at once with dt 0 |
| 0x10000 | random head tile per particle (when rows*cols > 1) |
| 0x20000 | head quad |
| 0x40000 | tail quad (neither head nor tail -> nothing drawn) |
| 0x80000 | independent x/y scaleVary |

**FBlock evaluation**:

- Timestamps int16, fractions of life: `t_key = ts / 32767`.
  `t = age / max(0.001, life + seed lifeVary/32767)`.
- **Linear**, no extrapolation (0x9793b0): 2 keys -> frac = t (timestamps
  ignored); 3 keys -> only ts[1] = m is used (`t < m` -> keys 0,1 at t/m, else
  1,2 at (t-m)/(1-m)); more -> binary search; 1 value -> constant.
- Values: colour C3Vector floats 0..255, **x = R, y = G, z = B**; alpha int16 /
  32767; scale C2Vector; cell uint16, lerped then **rounded to nearest**. No
  clamping (out-of-range bytes wrap).
- Vertex colour: D3DCOLOR 0xAARRGGBB, A = round(alpha x model alpha (clamped
  0..1) x 255); bytes reordered to RGBA on the GL device. **No
  premultiplication.**
- Fast path (0x979d60, internal 0x1000000) when every block has 1..3 keys
  sharing the same middle timestamp, cells <= 4 keys, lifeVary 0, scaleVary
  (0,0), scale x == y: a 2-segment table; colour keys rounded to bytes first.
  Quirk: a 4-key cell track's second segment starts at v1 with delta v3 - v2.
- **ParticleColor.dbc** (0x4ea9e0 -> 0x825410 -> 0x97a990): emitters whose
  R+0x2a == 11, 12 or 13 take start/mid/end colours from the row (columns
  0/1/2), keeping the colour FBlock's timing. **Missing row -> pure green
  (0, 255, 0).** Attached child models copy the override. Probably
  CreatureDisplayInfo.particleColorID (low).

**Size, twinkle, spin**:

- Billboard corners (+-1, +-1) x (sx, sy): **scale is a half-extent**.
- Size = scale(t) x scaleVary factor x twinkle factor (x bone world scale
  with flag 0x20).
- scaleVary from a per-particle RNG reseeded from particle+0x1e (stable over
  life): `s = max(1e-4, 1 + randS vary.x)` on both axes; with 0x80000 x and y
  get independent factors with vary.x / vary.y.
- Twinkle: `idx = (int(twinkleSpeed age) + (particleAddress >> 5)) & 127` into
  a 128-entry rand01 table; hidden when twinklePercent < 1 and
  `table[idx] > twinklePercent`; size x `min + table[idx] (max - min)` always
  (so (0, 0) would give size 0).
- Spin: `angle = baseSpin + randS baseSpinVary + age (spin + randS spinVary)`
  radians, CCW in the view plane; used **only when spin or spinVary != 0**
  (baseSpin alone is ignored).
- Velocity-aligned head (0x4): X axis = normalize(projected -vel), X half-size
  = sx |v_screen| / |v_3D|.

**Tail quad** (0x97cc10..0x97d35c), built in view space:

- `len = tailLength` (seconds), or `min(tailLength, age)` with flag 0x400.
- `T = view3x3 (-vel len)`, `E = P + T`. If |T.xy|^2 <= 1/1296 -> a plain
  billboard with the tail cell.
- Else `perp = (-T.y sy, T.x sx) / |T.xy|` (half-width). Vertices P+perp uv
  (0,0), P-perp (0,1), E+perp (1,0), E-perp (1,1); U runs head -> tail; cell
  from the tail FBlock (0 if none; random tile does not apply).

**Emission geometry**:

- Plane: `pos = (randS areaLength/2, randS areaWidth/2, 0)` (full extents);
  `speed = speed (1 + randS speedVar)`; if zSource != 0
  `vel = normalize(pos - (0,0,zSource)) speed`, else polar = randS
  verticalRange (cone half-angle from +Z), az = randS horizontalRange
  (+- around +X), `vel = speed (sin p cos a, sin p sin a, cos p)`.
- Sphere: `r = areaLength + rand01 (areaWidth - areaLength)` (min/max radius),
  elevation = randS verticalRange (from the XY plane), az = randS
  horizontalRange, `pos = r (cos e cos a, cos e sin a, sin e)`; direction
  radial (or from zSource, or +Z with 0x100).
- Sphere spawner exact (0x981950, setters 0x981490/0x9814b0/0x9813e0/
  0x9814d0): `e = randS * verticalRange` (E+0x240), `a = randS *
  horizontalRange` (E+0x244), `r = areaLength + rand01 (areaWidth -
  areaLength)`; `pos = r (cos e cos a, cos e sin a, sin e)`; direction = that
  unit vector, or `normalize(pos.x, pos.y, pos.z - zSource)` when zSource != 0,
  or (0, 0, 1) with flag 0x100; `vel = dir * speed (1 + randS speedVar)`.
- **Emitter matrix** (CM2Model::UpdateParticleEmitter 0x830c3c..0x830d0c):
  `M = Rz * T(M2Particle.position) * boneMatrix * X`, where X is the matrix at
  `[scene]+0xc4` (0x4c2370) and Rz is a constant at 0xd411e0 with rows
  (0,1,0), (-1,0,0), (0,0,1). In row-vector convention a local point is first
  mapped `(x, y, z) -> (-y, x, z)`: **every particle emitter's local frame is
  rotated +90 degrees about Z relative to its bone**. Emitter X lies along bone
  +Y and emitter Y along bone -X; Z is unchanged. It applies to plane and
  sphere emitters, in world and model space alike (the same matrix is used for
  the spawn transform and as E+0x184).
- Unless flag 0x10, pos and vel go through that emitter world matrix (with
  scale).
- Update (0x979bb0): `vel += wind dt` while age < windTime; follow delta;
  `pos.xy += vel.xy dt; pos.z += vel.z dt - 0.5 g dt^2; vel.z -= g dt`
  (gravity scalar along -Z of the particle's space);
  `vel -= vel min(1, drag dt)`.

**Blending** (jump table 0x8344dc): blendingType 0..6 -> GxBlend Opaque,
AlphaKey, Alpha, NoAlphaAdd, Add, Mod, Mod2x. Effect Combiners_Mod
(tex x vertex colour). Alpha lives only in vertex alpha, so for NoAlphaAdd,
Mod and Mod2x the alpha track and model fade have **no visible effect**
(medium: GxBlend factors not re-checked in the device). Material E+0xd4 =
7 at start: bit0 = !(flag 0x1) lit, bit1 = !(flag 0x8) fogged, bit2 depth
write only for blendingType 0/1.

Render-state chain 0x8214e0 -> 0x81fb10: particles are never back-face
culled; lighting only when bit0 is set and `0xa45374[m2blend]` = 1
([1,1,1,1,1,0,0] - Mod and Mod2x particles are never lit); fog mode
`0xa45390[m2blend]` = [1,1,1,2,2,3,4].

**Lit particles** (flag 0x1 clear, CDiffuse_T1):

- Positions are pre-transformed on the CPU by
  `M = [emitter world, only with flag 0x10] * T(-origin) * W` (W = the
  device World transform, the modelview), and drawn with an identity World.
- The normal of every vertex of the emitter is W's row 2 (0x97a468): **world
  +Z in view space**. The billboard is shaded as an upward-facing surface,
  regardless of the quad's facing.
- VS: `n = normalize(N)`, `dir = sat(dot(n, -c12))`, point lights
  `ndl_i = max(0, dot(normalize(c21+i - P), n))`,
  `att_i = 1 / (c25 + c26 d + c27 d^2)`;
  `L = sat(dir c10 + c11 + sum ndl_i att_i c17+i)`;
  **colour = sat(vertexColour * (L.rgb, 1) + c29)**.
- Constants from the owning model's light object (0x873ca0, drawCtx+0x70):
  c10 diffuse (clamped <= 1), c11 ambient, c12 light direction, c17..c27
  point lights (0x872900); c29 probably material emissive, 0 (low).

**Quad layouts** - corners and UVs in vertex order (indices (0,1,2),
(3,2,1), counter-clockwise seen from +Z of the quad):

| Vertex | Corner | UV |
|---|---|---|
| v0 | (-1, +1) | (0, 0) |
| v1 | (-1, -1) | (0, 1) |
| v2 | (+1, +1) | (1, 0) |
| v3 | (+1, -1) | (1, 1) |

`UV = cellOrigin + uv (1/cols, 1/rows)`; spin rotates the corners, never the
UVs; sx, sy are half extents.

- **Camera billboard**: `P + (cx sx, cy sy, 0)` in view space; with spin
  `P + (cx sx cos - cy sy sin, cx sx sin + cy sy cos, 0)`.
- **Velocity aligned** (flag 0x4, when |vel|^2 > 2.4e-7; ignores spin):
  `D = -vel * M3x3`, `d = D.xy`, `L = |d|` (all corners collapse when L^2 <=
  2.4e-7); `c = d.x/L, s = d.y/L`, `sx' = sx L / |D|`; corner
  `A = cx sx', B = cy sy`, vertex `P + (A c - B s, A s + B c, 0)`. U along the
  projected -velocity: u 0 leading edge, u 1 trailing.
- **XY quad** (flag 0x1000): basis B = upper 3x3 of M. Without flag 0x10 that
  is only the view rotation, so the quad lies **in the world XY plane facing
  world +Z** (bone/emitter rotation not applied); with 0x10 it lies in the
  emitter's local XY plane (bone scale cancelled). Vertex
  `P + cx sx B.row0 + cy sy B.row1` (U along +X, V along -Y). With spin the
  basis is rotated about the plane normal by the spin angle (0x4c5820, sign
  medium).

Example, Spells\Shadow_Precast_Med_Base.m2 emitters 0/1 (flags 0x20129,
sphere, blendingType 4 = Add): colour keys are R,G,B; with additive SrcAlpha
the green start is weighted by alpha 0..0.2 - dim teal fading to purple.

---

## 5. Shadows

Two systems for unit shadows, chosen by `extShadowQuality` (a real 3.3.5a
CVar, default 0, levels 0..5, registered at 0x78e86f, value `[0xd43154]`,
setter 0x874210 validates against hardware caps). There is no "darken the
character instead" path.

### 5.1 Blob decal (`extShadowQuality == 0`, the default) - high

- Setup 0x7e4a40: `Textures\ShadowBlob.blp` (`[0xd38044]`) plus two procedural
  64x8 vertical ramp textures: **ShadowMod** `[0xd3803c]` (white at both ends,
  black in the middle two thirds, linear ramps; generator 0x7e3820) and
  ShadowAdd `[0xd38040]` (white RGB, alpha trapezoid of the same shape;
  0x7e36e0). CVar `shadowLOD` ("Unit shadow LOD", default 1, 0..1).
- Gate 0x7e49e0: model loaded, `!(model+0x10 & 0x4000)`, non-empty box,
  `extShadowQuality <= 0`, `shadowLOD == 1`. `projectedTextures` does not
  gate it.
- Who gets one (`CWorldScene::AddObject` 0x781a10, entry flag `+0x7c |= 0x800`):
  units and players, and corpses unless flagged; **not** game objects, dynamic
  objects, missiles, spell visuals, spell models; not creatures with
  type_flags 0x02000000 (HAS_NO_SHADOW_BLOB); not submerged units (stand
  state 9, 0x793a9d).
- Size (0x793980 over `[0xcdb0a0]`, box from `CGUnit_C::GetShadowBox`
  0x71ed80): the CreatureModelData GeoBox (record +0x44..+0x58), merged with
  the mount's GeoBox (z-shifted) when mounted; model bounds as fallback
  (0x82ced0).
- Projection 0x7e4480(box, model, 0.4): footprint centred on the model origin,
  each local corner clamped to [-5, 5] and scaled by the model scale, the four
  corners rotated with the model (the blob turns with the unit), reduced to a
  world XY AABB. Vertical range `[z - 1.667*dz, z + dz]`, dz = half box
  height. Texture matrices (0x7e2d60): stage 0 maps XY onto the blob
  texture, stage 1 maps Z onto the ShadowMod ramp - the blob fades out above
  and below the unit's feet.
- Colour: white RGB, alpha = model alpha (`model+0x178`) x 255. No distance
  fade found. Fog off.
- **Receivers**: the client gathers real world triangles inside the box and
  redraws them with the decal (0x7e4370 -> 0x7a6af0, query flags 0x220122):
  - terrain triangles (0x7a6830, 0x7a6630);
  - WMO group triangles through the group BSP (0x7a6940 -> 0x7aef00 ->
    0x7cb7b0), skipping unloaded groups, groups with MOGP flag 0x80, and
    faces with MOPY flags & 0x88 (collision-only/invisible);
  - **not** M2s.
- Draw state:

  | State | Value |
  |---|---|
  | BlendingMode | Mod (dst x src) |
  | DepthWrite | 0 |
  | DepthTest | on (inherited) |
  | PolygonOffset | 0.4 x 2 / 32768 (0x763c70) |
  | Lighting, Fog | 0 |
  | Texture0 / Texture1 | ShadowBlob / ShadowMod |
  | TexGen stages 0, 1 | from position |

- **Indoors**: no indoor/outdoor branch at all. Inside a WMO the blob lands on
  the WMO floor triangles exactly as outdoors on terrain.
- **Order**: drawn in `CWorldScene::Render` after terrain, WMO and sky
  (0x793980), before every M2 - so models, the unit itself included, cover it
  and are never darkened by it; before water.

### 5.2 Shadow maps (`extShadowQuality` 1..5) - high on branches, medium on meaning

- Level -> mode table 0xb1d554 = {0, 1, 1, 2, 2, 3}; the blob is off.
- Rendered in 0x7bb570 -> 0x875f80, before terrain, so everything after can
  sample them. Callbacks installed at 0x7bd3a0 (light frustum 0x7bac10 /
  0x7bafd0, casters 0x7bd200, render 0x7bbc50 "ShadowMapRenderSL"); CVars
  `shadowCull`, `shadowScissor`, `shadowInstancing`, `hwPCF`.
- A group is **interior** when `!(MOGP & 0x48)` (neither EXTERIOR nor
  EXTERIOR_LIT); 0x2000 is not tested. Scene entries get `+0xc |= 2`
  (interior) or `|= 4` (exterior) (0x7bdfb5).
- **Map A** `[0xd43148]` - the character map, always at level >= 1. Casters:
  dynamic objects (PCs, NPCs) not flagged exterior and with `z <= player z + 4`
  (`[0xd25304]`, set in 0x7bb670), so units on other floors do not shadow
  yours.
- **Map B** `[0xd43250]` - the exterior sun map, only when exterior geometry
  is visible (`[0xadf59c] >= 0` or an exterior-lit group visible `[0xcd8778]`).
  Casters: levels 1-2 dynamic objects, 3-4 also animated doodads, 5 also static
  doodads. Interior objects cast into it only with entry flag 0x1000. Level >= 3
  adds the environment ShadowCache (0x874890, 0x874fb0).
- Receivers: interior WMO batches bind map A only (sampler 4, mode clamped to
  1); exterior batches bind map B with the full mode (+ ShadowCache above
  level 2). M2/units always receive (lighting flag 0x10, 0x780cd0); an M2 in an
  interior group also gets flag 0x8, takes its ambient and lights from the WMO,
  and is forced to map A only (0x81f1d0).
- Light direction: the sky light direction (`[0xce04a8]+0x7c`) with
  `z' = max(5z, -1.2)`, renormalised; maps centred on the camera target/player
  (0x7bb3e0). Cascade constants at 0xb1d520 (40/160/640, 4/16/1024) not
  resolved.

### 5.3 Terrain baked shadows (MCSH) - medium

- `mapShadows` (default 1) sets `[0xcd774c] |= 0x40`; `shadowLevel` (0..1,
  default 1) is a mip shift applied on restart.
- A chunk with the MCSH flag has its shadow bits unpacked together with the
  alpha map into a `64 >> shadowLevel` texture (`UnpackAlphaShadowBits`,
  0x7b98e9, 0x7b9a37). Fixed function: an extra pass with BlendingMode Alpha
  (0x7d12b9); shaders: Terrain2/3(_pcf) (0x7d3e10), dynamic maps on samplers
  5 and 6 (0x874660).

### 5.4 Related

- `projectedTextures` (default 0, `[0xcd7750] & 4`): generic projected decals
  using the same decal engine (query flags 0x200122), drawn after M2 pass 0
  (0x4f8a40).
- Footprints (0x79fcc0) use the ShadowMod texture.
- 0x7e5120 / 0x7e5580 are nameplates, not shadows.

---

## 6. Textures

Extracted shaders used for reading: `Data/expansions/wotlk/shaders/`
(`effects/*.wfx`, `pixel/*`, `vertex/*`; the ARB profiles are text). Dumper:
`build-dev/scratch/particles_re/m2tex/blsdump.py`.

### 6.1 M2 - high unless marked

M2 always draws through vertex + pixel shaders (the fixed-function path exists
but M2 options always set the shader bit 8, 0x402904). At most 2 texture
stages. One effect per batch, named VS name + PS name.

**Shader id rewrite at skin load** (0x836980, from 0x837b52):

- `shader_id & 0x8000`: kept.
- **Model header flag 0x08 clear: the file's shader_id is ignored** and rebuilt:
  stage-0 op = Mod if the material blend != 0 else Opaque; env bit 0x80 if
  texCoordCombo > 2 (i.e. -1); stage 1 Opaque; `|= 0x4000` if texCoordCombo
  == 1. `shader = op0 << 4`.
- Header flag 0x08 set: op_i = `textureCombinerCombos[shader_id + i]`;
  stage 0 forced to Opaque when the material blend is 0; env bit per stage when
  its texCoordCombo > 2; 0x4000 if the last texture's coord == 1.
  `shader = op0 << 4 | op1`.

Op codes: 0 Opaque, 1 Mod, 2 Decal, 3 Add, 4 Mod2x, 5 Fade, 6 Mod2xNA, 7 AddNA.
Bits: op0 = `(s>>4)&7`, env0 = `s&0x80`, op1 = `s&7`, env1 = `s&8`.

**Effect** (0x836600, from 0x836c90). If no combination exists, it retries with
shader **0x11** (Combiners_Mod / Combiners_Mod_Mod).

Vertex shader:

| Textures | Condition | VS |
|---|---|---|
| 1 | env0 | Diffuse_Env |
| 1 | no env, texCoordCombo != 0 (UV1) | Diffuse_T2 |
| 1 | otherwise | Diffuse_T1 |
| 2 | no env / env1 / env0 / both | Diffuse_T1_T2 / Diffuse_T1_Env / Diffuse_Env_T2 / Diffuse_Env_Env |
| 0x8000 batch | | Diffuse_T1_Env |

Pixel shader, 1 texture: op0 0..5 -> Opaque, Mod, Decal, Add, Mod2x, Fade;
6, 7 -> Mod.

Pixel shader, 2 textures (`-` = none, falls back to 0x11):

| op0 \ op1 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 0 Opaque | Opaque_Opaque | Opaque_Mod | Opaque_Mod | Opaque_Add | Opaque_Mod2x | Opaque_Mod | Opaque_Mod2xNA | Opaque_AddNA |
| 1 Mod | Mod_Opaque | Mod_Mod | Mod_Mod | Mod_Add | Mod_Mod2x | Mod_Mod | Mod_Mod2xNA | Mod_AddNA |
| 3 Add | - | Add_Mod | - | - | - | - | - | - |
| 4 Mod2x | - | Mod_Mod2x | - | - | Mod2x_Mod2x | - | - | - |
| 2, 5, 6, 7 | - | - | - | - | - | - | - | - |

0x8000 batches: 0x8001 Opaque_Mod2xNA_Alpha, 0x8002 Opaque_AddAlpha,
0x8003 Opaque_AddAlpha_Alpha; 0x8000 itself is not drawn. Add_Add,
Add_Opaque, Add_Mod2x, Mod2x_Add and Mod2x_Opaque `.bls` files ship but no
code reaches them.

**Combiner maths** (arbfp1 perm 0; t0, t1 samples, v = vertex colour with
lighting, M2 colour and transparency already applied):

| Combiner | RGB | Alpha |
|---|---|---|
| Opaque | t0 v | v.a |
| Mod | t0 v | t0.a v.a |
| Decal | lerp(t0, v.rgb, v.a) | v.a |
| Add | t0 + v | t0.a + v.a |
| Mod2x | 2 t0 v | 2 t0.a v.a |
| Fade | lerp(v.rgb, t0, v.a) | v.a |
| Opaque_Opaque | t0 t1 v | v.a |
| Opaque_Mod | t0 t1 v | t1.a v.a |
| Opaque_Add | t0 v + t1 | v.a + t1.a |
| Opaque_Mod2x | 2 t0 t1 v | 2 t1.a v.a |
| Opaque_Mod2xNA | 2 t0 t1 v | v.a |
| Opaque_AddNA | t0 v + t1 | v.a |
| Mod_Opaque | t0 t1 v | t0.a v.a |
| Mod_Mod | t0 t1 v | t0.a t1.a v.a |
| Mod_Add | t0 v + t1 | t0.a v.a + t1.a |
| Mod_Mod2x | 2 t0 t1 v | 2 t0.a t1.a v.a |
| Mod_Mod2xNA | 2 t0 t1 v | t0.a v.a |
| Mod_AddNA | t0 v + t1 | t0.a v.a |
| Add_Mod | (v + t0) t1 | (v.a + t0.a) t1.a |
| Mod2x_Mod2x | 4 t0 t1 v | 4 t0.a t1.a v.a |
| Opaque_Mod2xNA_Alpha | v lerp(2 t0 t1, t0, t0.a) | v.a |
| Opaque_AddAlpha | t0 v + t1 t1.a | v.a |
| Opaque_AddAlpha_Alpha | v (t0 + (1 - t0.a) t1 t1.a) | v.a |

Other PS permutations multiply in shadow-map lighting (textures 4..7, PCF).
PS perm = shadowMaps(0..3) + 4 hwPCF + 8 alphaTestInShader (KIL; then AlphaRef
state 0). VS perm = lit + 2 (pointLights 0..4 + 5 (min(bones,2) + 3
min(shadowMaps,2))) = 90 programs.

**Vertex colour** (VS): unlit `sat(c28 + c29)`; lit
`sat((sat(N.-L) c10 + c11 + sum point_i) c28 + c29)`, point light
`max(N.L_i, 0) c17 / (c25 + c26 d + c27 d^2)`. c10/c11/c12 light colour,
ambient, direction; c28 MatDiffuse, c29 MatEmissive; c30 fog (linear fog
factor `clamp(pow(max(z c30.x + c30.y, 0), c30.z))`).

- MatDiffuse.rgb = model tint (+0x1a0) x M2Color[colorIndex].rgb;
  MatEmissive.rgb = model emissive (+0x1ac).
- Unlit batches (material flag 1, or Mod/Mod2x): diffuse added into emissive,
  diffuse 0. Mod: diffuse 0, emissive 1. Mod2x: diffuse 0, emissive 0.5.
- MatDiffuse.a = model alpha (+0x19c) x M2Color.alpha x
  transparency[transparencyLookup[textureWeightComboIndex]] - **one weight per
  batch**, only when textureCount > 0 (0x821ea3).

**Per-texture parameters** (0x81f450):

- tex_i = instance.textures[textureLookup[textureComboIndex + i]] (state
  0x15 + i); unused stages null.
- **Wrap** from M2Texture.flags (1 = wrap U, 2 = wrap V), set on the texture
  object on every draw (`GxTexSetWrap`, 0x681450); clamp otherwise.
- **UV animation** tt = texTransformLookup[batch +0x16 + i]; env stages use
  identity (coords generated in the VS). Matrix (0x82d6f0), pivot (0.5, 0.5):
  `uv' = rotate_about_centre(scale_about_centre(uv + t))`; uploaded as two
  columns to c[6 + 2 slot]. Slot = stage, except a 1-texture UV1 batch uses
  slot 1.
- **Quirk**: Diffuse_Env_T2 samples its second texture with UV0 and the
  slot-0 (identity) matrix - that texture gets UV0 and no UV animation.
- An M2Texture with an empty filename (length <= 1) gets a solid white
  placeholder (0x4b9550).
- **Replaceable textures**: `CM2Model::SetReplaceableTexture(type, tex)`
  (0x825260) swaps every instance texture whose M2Texture.type matches, and
  also ribbons (0x97fad0) and particle emitters using that type (0x978c40).
  Type values per format docs (not checked in code).

**Environment map** (VS, view space): `u = normalize(viewPos)`,
`n = normalize(modelview N)` (no inverse transpose), `r = 2 n (n.u) - u`,
`uv = r.xy / |r + (0,0,1)| * 0.5 + 0.5` - the sphere-map formula with the
reflection negated.

**Particles and ribbons**:

- Atlas: rows (M2Particle +0x30) and cols (+0x32), both powers of two.
  `u0 = (tile & (cols-1)) / cols`, `v0 = (tile >> log2 cols) / rows`. Tile from
  the head/tail cell tracks (0x979560); M2 flag 0x10000 with more than one
  cell picks a random tile at spawn (0x979f8e).
- One texture per emitter: `shared.textures[M2Particle +0x16]` directly, no
  lookup (0x833ed5); no multi-texture particles in this build.
- Particle blendingType 0..6 -> the M2 blend 0..6 -> table 0xa453b0 (3.3).
  **Depth write only for blend 0 and 1.** Always two-sided. Effect "Particle"
  (CDiffuse_T1 + Combiners_Mod) or "Particle_Unlit" (Color_T1 +
  Combiners_Mod) selected by M2 flag 0x1 - as decoded, 0x1 = unlit, the
  opposite of wowdev's "lit" (medium). Flag 0x8 -> unfogged (medium). Colour is
  per vertex.
- Ribbons: material = materials[materialIndices[0]], always Particle_Unlit.

### 6.2 Samplers and texture CVars - high unless marked

- Filter, wrap and anisotropy live **on the texture object** (CGxTexFlags,
  0x681be0): bits 0-2 filter, 3 wrap U, 4 wrap V, 9-13 max anisotropy.
  `EGxTexFilter`: 0 Nearest, 1 Linear, 2 NearestMipNearest, 3 LinearMipNearest,
  4 LinearMipLinear, 5 Anisotropic.
- `textureFilteringMode` (default 1) -> global filter/anisotropy at startup
  (0x4048f0), one level lower if the device lacks support:

  | Mode | Filter | Anisotropy |
  |---|---|---|
  | 0 | LinearMipNearest (bilinear) | 1 |
  | 1 | LinearMipLinear (trilinear) | 1 |
  | 2..5 | Anisotropic | 2, 4, 8, 16 |

- `texLodBias` (default 0, -1..1): validated, never applied in this build
  (medium-high).
- `baseMip` (0/1): drops the top mip level and reloads textures (0x78df00).
- No per-material filter or LOD bias; mipmaps are the BLP's own.

### 6.3 Terrain (ADT chunks) - high unless marked

Shader dumps: `build-dev/scratch/particles_re/worldtex/shd/*.txt`.

- **One pass, one shader** (0x7989c0, shader pick 0x7d3e10 -> 0x79e5c0):
  layer i on Texture i (wrap), the combined alpha+shadow texture on the next
  unit; BlendingMode Opaque. Pixel shader "Terrain1", 32 permutations:
  `env + 2*(nLayers-1) + 8*bigAlpha + 16*hasUnlitLayer`. Terrain2 /
  Terrain2_pcf / Terrain3 = the same plus the real-time shadow map (sampler 5,
  5 taps, fading at the map edge). Terrain0 / Terrain0_env = one-layer-per-pass
  fallback; Terrain1w* only for fixed-function / ps 1.x profiles.
- **Alpha texture**: R = layer 1 alpha, G = layer 2, B = layer 3,
  A = baked shadow (MCSH).
- **Blend** (from the shader text), on RGBA (texture alpha = specular mask is
  blended too):
  - 4-bit maps (WDT MPHD 0x4 clear): `c = L0; c = lerp(c, L1, R);
    c = lerp(c, L2, G); c = lerp(c, L3, B)`.
  - Big alpha (MPHD 0x4 set): `c = L0 (1 - sat(R+G+B)) + L1 R + L2 G + L3 B`.
- **Final colour**:
  `out.rgb = vcolor.rgb * c.rgb * 2 * (0.7 + 0.3 A) + c.a * specular * A`,
  `out.a = 1`. A shadowed texel is 70% bright and has no specular.
- **MCLY flags**:
  - 0x80: unlit layer - raw texture, no lighting, no vertex colour, no x2;
    shadow and specular still apply (permutations 16..31, 0x7d2a14).
  - 0x400: layer 0 sampled as a cube map with reflection texgen (pixel
    shaders only; MTXF flag 1 marks the cube texture; black without shaders).
  - 0x40 animated: direction = `flags & 7` from
    (-1,0) (-1,1) (0,1) (1,1) (1,0) (1,-1) (0,-1) (-1,-1) (diagonals not
    normalised), speed = `(flags >> 3) & 7`; offset `+= dir * seconds`,
    scaled by `(1/0.24) / {64,48,32,16,8,4,2,1}[speed]`, applied as a texture
    matrix translation (roughly `1/div` repeats per second; medium).
- **UVs** from world position in the VS:
  `uv = (pos.yx - chunkOrigin) * scale + offset` (x/y swapped). Layer scale
  -0.24 = **8 repeats per chunk**; the alpha map 1/8 of that (one per chunk).
- **Alpha map unpack** (0x7b99b0, 0x7b9890): "TerrainBlend" 64x64, clamp,
  Linear, no mips. ARGB4444 for 4-bit maps (low nibble first, value n/15);
  big alpha ARGB8888 when `terrainAlphaBitDepth` == 8 (default). MCLY 0x200 =
  per-row RLE (high bit: fill `count & 0x7f`, else copy `count` bytes).
  MCSH: bit `x & 7` of byte `x >> 3`, LSB first; set = shadowed (A = 0),
  used only with MCNK flag 0x1 and `mapShadows` on.
  **Edge fix** (4-bit only, MCNK flag 0x8000 clear): column 63 = column 62,
  row 63 = row 62.
- Half-resolution 32x32 alpha (point-sampled) when `shadowLevel != 0` (the
  default is 1) or beyond ~777 yards (medium on the visible effect).
- **Specular** (`specular` CVar): `<name>_s.blp` per layer, alpha = mask;
  `pow(N.H, 20) * specColour` in the VS.
- **Vertex colour**: without MCCV `min(ambient + diffuse sat(N.L), 1) * 0.5`
  (the PS doubles it); with MCCV (MPHD 0x2) `light * MCCV` without the 0.5,
  so MCCV 127 is neutral. Up to 3 point lights. Per-vertex fog.
- WDL low detail (0x7d5e70): unlit, fogged, untextured (medium).

### 6.4 WMO - high unless marked

- MOHD flag 0x2 -> "unified" (U) path 0x7a9380, effect table 0xd1c3d4;
  otherwise legacy table 0xd1c3f0 via 0x7ac6a0 (0x7ac9f0 for some groups).
- MOMT shader ids (all multiply by 2):

  | id | Effect | Pixel |
  |---|---|---|
  | 0 | Diffuse | `rgb = T0 vc 2`, `a = T0.a vc.a` |
  | 1 | Specular | as Opaque (specular never added) |
  | 2 | Metal | as Opaque |
  | 3 | Env | `rgb = T0 vc 2 + T1.rgb T0.a`, `a = vc.a`; T1 by view-space reflection |
  | 4 | Opaque | `rgb = T0 vc 2`, `a = vc.a` |
  | 5 | EnvMetal | `rgb = T0 vc 2 + T1.rgb T0.rgb T0.a` |
  | 6 | Composite (U only) | `lerp(T1, T0, MOCV2.a) vc 2`, T1 on UV set 2 |

  Shader 0 becomes 4 when the blend is 0 and texture 1 has no alpha (0x7ac7df).
- Vertex colour: legacy `sat(MOCV.rgb * sat(amb + dir N.L + points) + emissive)`
  (MOCV multiplies the lighting); unified
  `sat(sat(amb + dir N.L) * 0.5 + MOCV.rgb + emissive)` (MOCV added).
- Lighting modes (0x7a8b10): 0 none; 1 Light.dbc ambient + sun; 2 second set
  (MOMT 0x20 windows); 3 MOHD ambient, no sun.
- **Transition batches** (the first transBatchCount) drawn twice (medium):
  exterior-lit with SrcAlphaOpaque, then interior-lit with InvSrcAlphaAdd -
  `exteriorLit * MOCV.a + interiorLit * (1 - MOCV.a)`. Interior batches mode 0
  (legacy) or 3 (unified), exterior batches lit unless F_UNLIT.
- MOMT flags: 0x1 unlit, 0x2 unfogged, 0x4 two-sided, 0x10 emissive (SIDN
  colour), 0x40 / 0x80 clamp S / T.
- Emissive = saturating byte add of SIDN and `[0xd1befc]`, halved (shader
  doubles). `[0xd1befc]` is non-zero only for the mouse-over highlighted
  game object WMO (medium).
- Missing texture 2 -> solid 0xff808080 grey when `[0xce0494]` (high).
- **AlphaRef by blend** (table 0xad8b7c, everything):
  `[0, 224, 1, 1, 1, 1, 1, 0, ...]/255`.

### 6.5 Liquids - high unless marked

| LiquidType.materialID | Used by | Shader | No shaders |
|---|---|---|---|
| 1 | water, ocean | Water (WaterNoSpec when `[0xb23f68]` = 0) | WaterFFP |
| 2 | magma, slime | Magma | MagmaFFP |
| 3 | procedural water | ProcWater (ps >= 3) | ProcWaterFFP |

- Textures (0x8a2450): up to 6 per type; a `%d` name loads frames **1..30**;
  frame = `floor((timeMs % period) / period * nFrames)`, period = LiquidType
  int[1] or 1250 ms (30 frames per 1.25 s). "procedural" names are generated
  in code (Linear, clamp); the rest wrap with the global filter.
- **Water**: `rgb = vc.rgb T0.rgb + T1.rgb + T1.a (spec + 0.25)`,
  `a = vc.a T0.a`. T0 = the procedural depth texture (a 64-step alpha ramp
  from LightParams shallow alpha to deep alpha, coloured by the river/ocean
  colour; medium); T1 = the `%d` frame with a texture matrix rotated by
  float[1] and scaled by float[0]. `vc = (amb + dif N.L) * vertexColour`,
  specular `pow(N.H, e) * c`. Culling off, blend Alpha, fog on.
- **Magma/slime**: `rgb = vertexColour * T0`, `a = 1`, unlit, fogged.
- ProcWater: two scrolling normal maps, reflection/refraction cube maps with
  Fresnel, specular mask, `basicWaterHeightTex_%d`; scroll period
  `1000 * speed` ms.

### 6.6 Other global facts

- Every BLP loaded without load flag 1 (terrain, WMO, M2, liquid) gets the
  global `textureFilteringMode` filter (6.2).
- `baseMip` shifts width/height by 1 for mipmapped textures (0x683210).
- No sRGB handling (device gamma ramp only), no alpha-to-coverage (medium).

---

## 6b. Spell visual kits - high unless marked

DB globals: SpellVisualKit 0xad4a3c, SpellVisualEffectName (SVEN) 0xad4a18,
SpellVisualKitAreaModel 0xad4a60, SpellVisualKitModelAttach 0xad4a84,
SpellVisual 0xad4aa8. (Table 0xa2d3f0 is the vehicle-seat attachment map, not
a kit table.)

- Record layouts: SpellVisualKit (0x98 bytes): ID, StartAnimID, AnimID,
  +0x0c Head, +0x10 Chest, +0x14 Base, +0x18 LeftHand, +0x1c RightHand,
  +0x20 Breath, +0x24 LeftWeapon, +0x28 RightWeapon, +0x2c..+0x34 Special0-2,
  +0x38 World, +0x3c Sound, +0x40 Shake, ..., +0x94 Flags. SVEN: +8 FileName,
  +0xc AreaEffectSize, +0x10 Scale, +0x14 MinAllowedScale, +0x18
  MaxAllowedScale. ModelAttach: +4 ParentKit, +8 SVEN, +0xc AttachmentID
  (-1 = world placed), +0x10..+0x18 Offset, +0x1c Yaw, +0x20 Pitch, +0x24 Roll.
- **Column -> M2 attachment** (kit play 0x745230 -> CEffect 0x744790):

  | Column | Attachment |
  |---|---|
  | Head | 20 Head |
  | Chest | 34 Chest |
  | Base | 19 Base (world placed for play types 3 and 7) |
  | LeftHand / RightHand | 21 / 22 SpellLeft/RightHand |
  | Breath | 17 |
  | Special0..2 | 23..25 |
  | LeftWeapon / RightWeapon | 2 HandLeft / 1 HandRight on the unit's model (vfunc +0x100, 0x73a6c0); skipped when CreatureModelData.Flags & 0x10 |
  | World | world placed |
  | ModelAttach rows | their AttachmentID; -1 world placed |

- Attached columns are parented through 3.5 (`CEffect::UpdateAttachment`
  0x6f8c50 at activation and re-attach events) and **follow the unit every
  frame**. State kits (type 2) attach Base to the unit.
- World-placed effects (World column, ModelAttach -1, Base for play types 3/7)
  stay at the play's target position (or the unit's position at play time) and
  move only with a transport; exception effect flag 0x4000 (spell aura 26,
  medium) re-reads the object's position every frame.
- Child local matrix (set once): ModelAttach yaw/pitch/roll (yaw negated) and
  offset (0x6f84f0; exact offset signs unresolved), then uniform scale
  `s = CreatureModelData.AttachedEffectScale (units, else 1) * SVEN.Scale`,
  with `s * f` (f = scale of the parent's attachment bone) clamped to
  [SVEN.Min, SVEN.Max]. World effects: `max(1, 0.3 min(bbox x, y)) *
  CreatureModelData.WorldEffectScale * object scale * SVEN.Scale`, clamped
  (medium).
- Kit play types (kill switch 0x744bd0): 0 CastKit one-shot, 1 ImpactKit
  one-shot, 2 StateKit loop, 3 world placed (not killable), 4 PrecastKit loop,
  5 CasterImpactKit, 6 TargetImpactKit, 7 MissileTargetingKit (loop, world),
  8 StateDoneKit one-shot.
- **Lifetime**: on load the model plays animation 0 Stand (0x6f7680). State
  kits: loop Stand if kit flag 0x20, else Hold (158) if present, replayed
  forever. Precast/type 7: loop. One-shot kits: after the first cycle Decay
  (159) if present then finish, else freeze on the last frame and finish.
  Birth (127) is not used by kits.
- **Aura removal** (0x744bd0): `CEffect::Finish` immediately - mesh hidden at
  once, no Decay and no alpha fade, emitters stop, live particles finish their
  life, effect deleted once no particles remain; sound fades over 0.15 s.
- No rule hides an effect's mesh because it has emitters. Mesh suppression
  comes only from: batch alpha < 0.0001, shader 0x8000, missing texture,
  attachment visibility tracks, CEffect start delay, Finish, model flag 0x4000
  (setter not traced).

---

## 6d. Doodad and GameObject animation states - high unless marked

- **Map doodads (ADT MDDF, WMO MODD) always play Stand (0).** All three
  creation paths (0x7bda70, 0x7becd0, 0x7bef40) call SetAnimation
  0x832ab0(-1, 0, ...); the culling/animate path only toggles SetAnimating.
  There is **no time-of-day, world-state, area, phase, MDDF/MODD-flag or
  doodad-set rule** for map doodads. The client's only day/night test
  (0x4c9850: day = 05:30-21:00) is used by zone music and spell
  day/night-only checks.
- Example: Dalaran_LampWall01 (GameObjectDisplayInfo 8092) and
  Dalaran_Fountain_01 (8097, the Tirion statue in Opened) are **server-spawned
  GameObjects**, not placed in nd_dalaran.wmo (its MODD places only
  Dalaran_Fountain_02, Stand only).
- **Animation resolver** (0x826350, all M2s): play the id if the model has it,
  else follow AnimationData.FallbackID (flag 0x10 = play the fallback
  reversed, 0x20 = hold it); at the end of the chain Stand, else Closed, else
  the first sequence.

  | Id | Name | Flags | Fallback |
  |---|---|---|---|
  | 0 | Stand | 0x3 | 147 |
  | 146 | Close | 0x10 | 148 reversed |
  | 147 | Closed | 0x20 | 146 held |
  | 148 | Open | 0x13 | 146 reversed |
  | 149 | Opened | 0x23 | 148 held at end |

- **GameObject state machine** (GameObject_C.cpp), state -> animation table
  0xada938: 0 Spawn 145, 1 Closed 147, 2 Open 148, 3 Opened 149, 4 Close 146,
  5 Destroy 150, 6 151, 7 152. Driven by the GO state (GAMEOBJECT_BYTES_1
  byte 0) and progress P (high word of GAMEOBJECT_DYNAMIC, 0xFFFF = at rest;
  field naming medium).
  - On creation (0x70d600): GO state 0 ACTIVE -> Opened (Open if P !=
    0xFFFF); 1 READY -> Closed (Close if in progress); 2 -> 6 (5 in
    progress). **An object that loads open goes straight to Opened.**
  - On change (0x70d690): to 0 -> Open if old was 1 or in progress, else
    Opened; to 1 -> Close if in progress or old was 0, 7 if old was 2, else
    Closed; to 2 -> 5 or 6.
  - Missing sequences (0x70d1e0): Close missing -> Open reversed via the
    resolver, else Closed; Closed missing -> Close held, else Open at speed 0,
    else Stand; Open missing -> Close reversed, else 150 if present, else
    **Opened directly**; Opened missing -> Open held, else Close at speed 0,
    else 151.
  - Transitions start at `P/65535 * duration`; when they finish (0x70d7e0)
    Open -> Opened, Close -> Closed, 5 -> 6 (one shot, then hold). Doors also
    set collision from the state.
- **Resolver arithmetic** (0x826350): start `edi = +1, acc = 0`; per step
  flag 0x10 `acc += edi; edi = -edi`, flag 0x20 `acc += edi; edi = 0`
  (reverse before hold in one row; flags 1/2 ignored). Mode: edi > 0 -> 0
  normal, edi < 0 -> 1 reversed, edi == 0 -> 2 (acc <= 0, frozen at frame
  0) or 3 (acc > 0, frozen at the end). A chain that runs out (visited id, no
  row, id >= 0x1fa, self fallback) takes the default **with mode 0** - the
  accumulated flags are discarded. SetAnimation (0x826b00): modes 1/3 negate
  the speed (start at the end, play backwards), modes 2/3 force speed 0.
  Examples: Closed with only Close present -> mode 3; Open with only Close ->
  mode 1.
- **Loop or once**: M2 sequence flag 0x01 plays once to its end and stops
  (medium on the flag name); otherwise the time wraps every duration and an
  end event fires each cycle (0x832260, 0x831fc0).
- **No client time-of-day path for GameObjects** (verified): every reader of
  game time (0xd37f98; accessors 0x76c360 / 0x76cff0 / 0x76c1f0) is music,
  sound, the WorldFrame time struct, calendar, Lua GetGameTime, spell
  day/night checks, time sync or the sky; none is in the GameObject, M2
  animation or doodad code. GO animation/state changes come only from the
  server (GO state in BYTES_1, progress in DYNAMIC, custom-animation and
  despawn-animation opcodes, spawn) or player actions (gossip/quest UI,
  closing loot, casting on the object). GameObjectDisplayInfo has no
  time-related field; no AnimationData id has a day/night meaning. Lamps that
  light at night are a server decision (e.g. a game event changing GO state).
- Stand-only GameObject (e.g. a lift door whose Stand is the opening): READY
  and ACTIVE both end at **Stand, mode 0, speed 1, from time 0**, restarted
  when the state is applied; it loops unless Stand has sequence flag 0x01.
- Doodads inside a GameObject's own WMO get Open/Close from the GameObject
  (0x7b4170, filtered by doodad set), ending in Opened/Closed (0x70b2b0).

---

## 6c. Third-person camera collision - high unless marked

Units are yards (constants are foot fractions: 1/9 = 4 in, 2/9 = 8 in,
5/6 = 2.5 ft). Camera update 0x606f90; collision core 0x605d60; near-plane
box sweep 0x6059e0; liquid-surface avoidance 0x6061d0; world line test
0x77f310 -> 0x7a3b70 `(start, end, hitOut, t, flags)`.

- **Flags** (0x5fec50): `0x100171 | (cameraWaterCollision ? 0x20000 : 0)`,
  default 0x120171. 0x100 terrain, 0x10000/0x20000 liquid (flag-4 types /
  all surfaces), 0x100000 game objects (medium); 0x1 doodad collision,
  0x10 WMO collision, 0x20 WMO render geometry, 0x40 WMO no-camera-collide
  filter (low). Units are not tested.
- **Ray 1, look-at height under ceilings**: from `P + (0,0,5/6)` up to
  `P.z + max(H + k, 5/6 + 1/9)` (H = max(current, wanted height), k = flying
  mount offset); on a hit `height = max(length t - 1/9, 1/9)`; clamped to
  `min(current, max(5/6, unitCollisionHeight, clipped))`. Submerged with water
  collision: start just above the surface (`waterDepth + 2/9`).
- **Ray 2, the boom**: from `Q = P + (0,0,height)` back along -forward for
  `max(current, wanted distance)` (plus the flying-mount up offset); on a hit
  `distance *= t`. Shake and offsets are added only to the final position.
- **Near-plane box sweep** (medium on geometry): the near-plane rectangle
  (from near clip, aspect, FOV) swept from the camera toward Q; two passes,
  liquids at scale 1.0, solids at scale 1.75; `distance -= f (distance -
  near)`; `distance - near < 0.001` -> distance 0 (effectively first person).
- **Padding**: if the distance changed, `distance = max(distance - 1/9, 0)`;
  `D = min(distance, current)`.
- **Liquid surface** (always): a vertical probe +-2/9 around the camera
  (flags 0x20000); the camera is kept 2/9 above or below a surface, then the
  solid sweep re-runs.
- **Pull-in is instant** (placed at D every frame). If `current - D > 1/9`:
  `current = D + 0.111112`, start a **2.0 s cosine ease**
  `a + (b - a)(1 - cos(pi t))/2` back to the wanted distance (re-triggered
  every frame while blocked). The look-at height eases back the same way.
  Mouse-wheel zoom during the ease changes only the wanted distance.
- CVars: cameraDistanceMax 15 x cameraDistanceMaxFactor 1, hard cap 50; min 0;
  cameraDistanceMoveSpeed 8.33 yd/s (wheel); cameraDistanceSmoothSpeed 8.33
  (programmatic distance changes only); cameraHeightSmoothSpeed 1.2;
  cameraWaterCollision 1; cameraTerrainTilt 0 (tilt +-20 deg, sampled every
  100 ms); pitch/yaw move 90/180, smooth 45/180.
- **Look-at point** (0x604e00, medium): attachment 0x11 of the stand-state
  animation + 0.0972222, times scale (fallback bbox height x 0.9), at least
  0.8333; separate swimming / normal / close (wanted distance < 1.8315)
  heights eased at cameraHeightSmoothSpeed. cameraPivot is not the target
  height.
- **Close fade**: when `D - near < 1.8315` the player model alpha is
  `255 * cosEase((D - near - 1/360) / (1.8315 - 1/360))`, 0 at <= 1/360.

---

## 7. Differences in wowee to resolve

Recommendations only; each needs its own change.

1. **Characters are a separate pass before the M2s.** In the client, units and
   doodads share one M2 scene: opaque in pass 0, blended batches sorted back to
   front together. A character's blended part drawn before an M2 behind it can
   be painted over.
2. **Blended M2 batches are not split by the water plane.** The client sorts
   blended M2 batches, particles and ribbons into "above water" (after the
   water) and "below water" (before the water), with a clip plane for models
   that straddle it, and reverses that order under water.
3. **Blended batches keep depth write** in the client (unless material flag
   0x10), for M2 and WMO alike; particles do not write depth. wowee's own
   comments say its blended WMO and M2 pixels leave no depth - not yet checked
   batch by batch.
4. **Sky**: the client draws it after terrain and WMO, with the world in depth
   [0, 0.94] and the sky in [0.999, 1.0], scissored to the visible exterior.
   wowee draws the sky after terrain only, because its blended WMO batches
   leave no depth.
5. **Particles**: pool per emitter by the formula in 4.1 instead of a cap per
   instance; distance LOD on emission; per-emitter draw cap; no fixed shared
   buffer limit; frozen when culled.
6. **Character shadows indoors**: at default settings the client draws a blob
   decal multiplied onto the actual terrain and WMO floor triangles under the
   unit, the same indoors and outdoors, before any M2 is drawn. With shadow
   maps on, interiors use a separate character-only map whose casters are
   limited to `z <= player z + 4`; the sun map is used only where exterior
   geometry is visible.
7. **Portal culling** narrows to screen rectangles (portal bounding rect
   intersected with the parent rect, depth limit 10), and the outdoors seen
   from inside is drawn in one pass with the union rect of exterior portals.
