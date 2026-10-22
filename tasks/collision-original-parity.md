# Original-client movement and collision parity

Requirement chosen by the user on 2026-10-22: graphics may change; gameplay
behaviour must match the original client. See
[the governing principle](principles/original-gameplay-behaviour.md).

Status: contact/surface phases and the combined supplied-state world collector
have original-executable comparisons. The unattached request-to-world path,
frame projection/application, height-command queue and restricted frame
scheduler are verified offline. The native collision/ADT selection passes
41/41 tests. Full behavioural equivalence is not established and the player
solver is not replaced. Production world lifecycle, remaining input commands,
unit event delivery, interpolation, transport/spline movement and outgoing
packet bytes remain open. See `docs/original-collision-reference.md` for the
implemented boundaries and checks.
This plan supersedes the audit's generic solver
redesign as the acceptance target. A common collision API remains useful, but
its behaviour must be derived from the original implementation.

## Reference and scope

Start with the verified local WoW 3.3.5a build 12340 recorded in
`docs/collision-audit-2026-10-22.md`. Identify build-specific behaviour before
enabling the replacement for other supported expansions.

Collision parity includes input geometry, body dimensions, contact solving,
movement state and outgoing protocol state. It cannot be isolated to
`CollisionWorld::sweepMover`: CameraController currently owns grounding and
much of physics, while MovementHandler constructs the packets.

## 1. Establish executable reference comparisons

- Preserve the audited binary hash, function addresses, relevant reconstructed
  control flow and all unresolved dependencies in reproducible tooling.
- Establish an offline way to execute the original routines against synthetic
  geometry, or collect original-client trajectories for the same controlled
  scenes and timed commands. A new implementation cannot generate its own
  expected answers. If only one route works, state its coverage limits.
- Include intermediate state and travel/contact outputs, not just an endpoint.
- Verify source of numerical differences: original x87 intermediates versus
  the target compiler's float operations, contact tolerances and facet order.
  Do not choose a broad positional tolerance that conceals different contacts
  or movement-state transitions.

Deliverable: repeatable reference runner/fixtures and a differential report.
This is the first dependency; no claim of parity before it exists.

## 2. Reproduce the original collision inputs

- Recover world facet gathering, masks, BSP participation, terrain holes,
  winding and normal transforms, including tilted/scaled M2 and transports.
- Preserve behaviourally significant candidate ordering. Spatial acceleration
  must not change contact choice at ties.
- Carry actual mover radius, height, scale and step budget. Apply the server's
  collision-height update as state, not merely an echoed acknowledgement.
- Determine the original response to unavailable collision data. Keep render
  streaming asynchronous without inventing a floor or changing physics time.

Deliverable: matching input facets and dimensions for reference scenarios.
Cost: extend collision queries and movement state across the existing layer
boundary; merely reusing current floor/wall lists is insufficient.

## 3. Reproduce contact and surface movement

Use verified versions of the original volume construction, polygon clipping,
`DistanceToMove`, surface classification, obstacle redirection, `AttemptStepUp`
and `TraceSurface`. Preserve the original sequence and its conditional paths.

Cover first contact, sliding, corners, upward head contact, up/forward/down
step attempts, signed slope normals and loss of support. Test the dimensions
and step budget from the original, not the dimensions in the old audit probe.

Deliverable: matching contact, displacement and support transitions in an
offline implementation linked without rendering or a world-server connection.
Cost: this replaces the existing solver semantics; it is not threshold tuning.

## 4. Reproduce time and movement-state transitions

- Recover the original subdivision of elapsed time and transitions between
  grounded, jumping, falling, landing, swimming and flying.
- Compare knockback and other forced movement, water walking, hovering,
  transport attachment/detachment and moving-platform frames of reference.
- Audit existing coyote time, jump buffering, gravity integration, drag,
  `min(deltaTime, 1/30)`, landing thresholds and floor rescue against evidence;
  no existing heuristic is retained merely because it feels acceptable.
- Keep character simulation out of camera orbit, render interpolation and
  visual quality settings. The camera consumes the simulated pose.

Deliverable: matched complete trajectories under the same timed commands,
including different frame schedules and boundary cases.
Cost: substantial replacement of CameraController's movement/grounding and
coordination with game-owned movement state, not just collision-library edits.

## 5. Validate the server-visible contract

Drive native movement through GameHarness where applicable and an offline
movement integration harness. Compare the observed event sequence with the
reference: start/stop, jump, fall/landing, heartbeat, forced movement and
collision-height acknowledgement, including positions and transport fields.

Record what is normalized in comparisons (session identity, clock origin)
and preserve meaningful relative time and movement values. A server accepting
one session is a smoke test, not proof of original behaviour or ban safety.

Deliverable: no unexplained trajectory/state/packet divergence in the agreed
matrix, with known coverage limits stated explicitly.

## 6. Integrate and remove replaced heuristics

Connect the verified mover to normal client input and the movement domain;
remove competing floor arbitration, artificial adhesion, rescue and old
collision responses after their responsibilities are covered. Retain the
repository's dependency boundaries and per-character state ownership.

Complete the relevant native tests and `sweep_guard`, then check representative
world scenes and performance with real data. Do not enable an incomplete
replacement in ordinary gameplay or silently mix the two movement models.

Deliverable: production path uses the verified behaviour; reference regressions
remain runnable; unsupported cases remain visible in the completion report.

## Progress

- [x] Audit current implementation and inspect selected original functions.
- [x] Reproduce current collision defects through the public CollisionWorld API.
- [x] Record the user's original-behaviour requirement and acceptance criteria.
- [x] Executable original-reference comparisons for DistanceToMove on supplied facets.
- [x] Three legacy query regressions reproduced before fixes and corrected.
- [ ] Equivalent collision geometry and mover parameters.
- [x] Supplied-geometry contact/surface phases against the original executable.
  DistanceToMove matches 1306 calls; TraceSurface, stepping and its speculative
  fall checks match 1069 calls, including consecutive-frame stair trajectories.
  This does not certify world facet collection or the other movement phases.
- [ ] Equivalent full movement trajectories and state transitions.
- [ ] Equivalent server-visible movement.
- [ ] Verified production integration and removal of competing heuristics.

Geometry progress: WMO MOBN/MOBR are now retained through loading and model
preparation. Independent original-code comparisons cover BSP traversal/masks,
model facet transforms and terrain chunk queries including holes. SSE references
execute host RSQRTSS because the emulator's default approximation is incorrect.
World placement ordering, M2 facets, liquids, impassable boundaries and streaming
still need to be connected and compared before changing player simulation.

Falling progress: original Fall/FallDown matches 2993 calls, including jump/landing
sequences, ceiling resets and deferred movement actions. This remains a supplied-
geometry phase; full frame dispatch, transports and packet delivery are pending.

Swimming/flight progress: resident-facet Swim matches 2848 original calls,
including water-boundary jump restrictions, solid-contact liquid suppression
and consecutive frames. Transport detach, active splines and the frame
dispatcher remain outside this phase. Eleven native collision tests pass.

Hover/ground progress: 2256 HoverMove calls and 1065 GroundNormal queries
match original machine code on Linux, native MinGW and ASan/UBSan. The frame
dispatcher, world-facet acquisition and protocol integration are still pending.

M2 progress: fixed the three-float authored-normal loader stride and retained
normals through collision shape creation. Original GetCollisionFacets agrees
in 1512 cases on Linux, native Windows and ASan/UBSan. This closes model-local
facet generation, not whole-world collection order or movement integration.

World-query progress: impassable boundaries agree in 1017 cases; terrain
liquid facet selection agrees in 1014 x87/SSE cases; mover query extents and
participation flags agree in 2500 cases. These APIs still need the world
collector, repeated-query path and production dispatcher, not only phase probes.

Request-dispatch progress: 3413 cases compare the unattached CollideRequestMove
path, repeated geometry queries, liquid inversion, phase changes and event
boundary calls. Query preparation/refresh has 3700 cases. Missing geometry in
FallDown now follows the original skipped-time branch. Real world gathering,
unit event delivery, active splines, transports and production integration are
still required; this milestone does not close the overall checklist.

ADT/flight progress: MCRF placement ordinals and MHDR-controlled MFBO bounds
are retained, with explicit failure for corrupt references. Flight-bound facets
match 1608 original calls; the native collision/ADT selection passes 22/22.
Remaining loader gaps include original liquid vertex material formats, raw MCLQ
participation and world links. The production movement path remains unchanged.


Authored liquid/world progress: retained raw MCLQ/MH2O layers separately from
visual height fallbacks, including legacy tile flags, material vertex formats,
rectangle masks and original layer order. MCNK identity now comes from its
header coordinates rather than file order. Immutable terrain snapshots carry
MCRF references, MDDF/MODF placements, flight bounds and raw liquid inputs.
Original liquid vertex readers/CreateVertXY agree in 1800 cases. Original
GetFacets dispatch agrees in 2130 cases, and the loaded terrain/liquid portion
of GetChunkFacets agrees in 650 cases. The dispatcher is abstract until all
world object sources are supplied; it is not a production movement replacement.

WMO liquid progress: GenLiquidVerts/GetLiquidFacets agree in 908 cases on Linux,
native MinGW and ASan/UBSan; host-SSE comparisons also pass. Tests cover exact
cell seams, tile participation, transformed planes and the 8192-vertex cull
limit. The WMO cell multiplier is one float above the terrain multiplier; using
the terrain value failed 10 seam cases before correction. Liquid-only groups
now survive loading and shape preparation. Every source group is retained in
file order, with authored bounds, BSP, MODR references and liquid vertices,
independently of renderable batches. Truncated MLIQ no longer fabricates wet
cells; both loader regressions failed against the old loader before passing.

Unavailable terrain progress: the absent/asynchronous-area branch of
GetChunkFacets, including its AddAaBoxFacets barriers, agrees in 1254 cases on
Linux, native Windows and ASan/UBSan. WDT presence, local-control participation,
near-body rejection, original cell coordinates, touching bounds and facet
order are covered. A resident area's missing chunk remains a separate failure.
This does not validate WMO/doodad availability or the streaming owner's wiring.

Validation for this increment: 29/29 native collision/ADT tests pass; changed
WMO model preparation and renderer objects compile. The invalid MH2O rectangle
regression first produced an ASan heap-buffer-overflow, then passed after
rejecting the rectangle before visual mask construction. All seven ADT
collision tests pass (174 assertions), as do WMO loader/shape regressions.
No game account or server was used. Production movement is still the old path.
Remaining work includes MOGI/world placement selection and order, linked doodads
and dynamic objects, mover dimensions, transports/splines, unit event delivery
and packet comparisons. The overall acceptance checklist remains open.

WMO model assembly progress: `appendWmoModelFacets` matches 682 executions of
GetTris/GetLiquidFacets, including MOGI selection independent of MOGP, authored
group order, the separate solid/liquid passes, 31 usable batches and strict
face/index pool limits. Liquid rectangles reserve all clipped indices before
filtering dry cells. Preparation now retains MOGI bounds beside every source
group; missing MOGI is reported rather than replaced by visual mesh bounds.

World WMO progress: `appendWorldWmoFacets` matches 1693 original GetMapObjFacets
executions. Coverage includes rotated/scaled/translated placements, x87 center
rounding and per-axis AABB stores, root/group bounds, disabled placements,
destructible proxy rejection, partial output before failure, local-control
barriers and their different whole-building/group readiness rules. Linked
M2/entity calls in this particular oracle are supplied services: call order,
arguments and facet counts are compared, but their bodies are not executed
there. Separate collectors below execute those bodies independently.

Linked M2 progress: `appendDoodadFacets` matches 1248 GetDoodadDefFacets
executions with real M2 facet generation and original box barriers. This covers
query-tag deduplication across lists, marking misses, GUID-dependent masks,
disabled/missing models and transformed shared-model bounds while preparation
is pending. No per-model exemptions or visual bounds substitutions were added.

Dynamic entity progress: `appendEntityFacets` matches 1095
QueryDestructibleFacets executions with a supplied collision-data callback and
real M2 facet generation. The first link without static flag 2 ends the list;
hidden/visited entries skip instead. Callback failure still marks the query
tag, while callback absence does not. Parameters, callback order and returned
facet ownership are compared. The real game's collision-data provider still
needs to be wired and validated.

The four new independent fixture sets contain 4718 cases. Linux comparisons
and ASan/UBSan pass; WMO model/world comparisons also pass with host RSQRTSS.
This closes additional collection rules, not the world owner's lifecycle,
original placement/link construction, combined collector integration, mover
sizes, active splines/transports or network reporting. Production player
movement remains on the old path until those contracts are verified.

Validation for the object-collection increment: 33/33 native collision/ADT tests
pass, WMO model/world host-SSE results also match on native Windows, affected
renderer objects compile, and all available sweep_guard checks remain within
their ceilings. No live client or player account was used.

Combined supplied-world collection now matches 1072 original GetFacets calls,
executing WMO, terrain, linked M2 and entity-list collection together. Shared
objects occur in multiple lists, with successive query tags, missing areas and
partial groups. The only substituted geometry service is the dynamic entity's
collision-data callback. The real streaming/link lifecycle remains separate.

Mover dimension rules now match 1884 original executions of SetCollisionBox /
SetCollisionHeight followed by real query-envelope generation or refresh. The
model scale, reference scale, forced height and controlled-player override rules
are retained. Server height events still need ownership, queue timing and ACK
integration; this is not yet a live fix for the ignored-height packet handler.

Falling-edge precision: 3705 new original-code cases cover thin tilted platforms,
nearby walls, large coordinates, jump-peak boundaries and the falling-far flag.
Before correction, 43 edge trajectories and 75 falling-far boundary cases failed.
Original x87 intermediate/store boundaries are now retained in edge selection,
plane intersection, steep support evaluation and fall threshold comparisons.
All 3705 Linux comparisons pass. This replaces rounding errors, not the original
movement rules. Production movement and outgoing-packet parity remain open.

Combined movement/world progress: 1072 cases execute CollideRequestMove and real
GetFacets together in the original binary, comparing resulting movement state,
query order/extents, readiness and movement-event boundary calls. These pass on
Linux and native Windows. Synthetic authored geometry and link state are supplied;
unit event/network delivery is observed rather than executed. The old world-query
reference still matches all 1072 cases after sharing its setup with this oracle.

Validation: 37/37 native collision/ADT tests pass. The expanded 3705-case falling
regression also passes ASan/UBSan. Dimensions and standalone combined-world
comparisons passed their respective sanitizer probes. No live account was used.

Projection/frame progress: original PlotUnitMovement (0x987b50) and its complete
mode table agree in 4096 cases, including turning, pitching, spiral motion,
ascent/descent, optional output pointers and 32-bit time boundaries. A separate
host FSINCOS oracle also matches all cases; projection passes ASan/UBSan.

ApplyMovement (0x6eac40) now joins projection, local/remote anchor timing,
RequestMove and the real world collector. All 1444 cases match, including
remote delay ramps, counter wrap and world-edge admission. A premature target
store in the initial port caused 552 divergences; preserving the original x87
anchor-plus-move-minus-position sequence resolves them. Thirty-frame sequences
include landing and subsequent movement. This entry explicitly
excludes interpolation, attached transports, splines and queued input delivery.
The frame oracle supplies thread globals and an empty game-object registry;
unit SkipTime/event delivery is observed at its boundary. It does not validate
heartbeat packet scheduling or the complete ExecuteMovement loop.

The earlier combined movement/world oracle also passes ASan/UBSan, and the full
sweep remains within its ceilings. The 1444-frame/4096-projection increment
passes the 39-test native collision/ADT selection. The overall acceptance
checklist remains open, and ordinary gameplay still uses the previous path.

Final frame validation: all 1444 cases, including the six thirty-frame sequences,
pass native Windows and ASan/UBSan. All 39 native collision/ADT tests pass and
sweep_guard remains within every ceiling (unavailable optional checks skip).


### 2026-10-23 — Height queue and execution schedule

Verified height command ordering and application-before-notification through
36,864 original UpdatePlayerMovement calls in 768 scenarios. Verified the
ExecuteMovement schedule joined to the frame solver and supplied-state world
collector in 1444 cases, including six thirty-frame sequences. This includes
250ms frame limits, rollover, timed height changes, heartbeat deadlines,
unavailable geometry and passenger deferral. Linux ASan/UBSan and native
Windows pass; the full native collision/ADT selection is now 41/41.

Scope remains explicit: heartbeat preparation is a successful supplied service,
not verified wire bytes. Unit events, height notification delivery and mover
retirement are observed boundaries; their remaining side effects are not
silently treated as implemented. No change is enabled in player control.
Next work is the remaining original input/state commands, their synchronous
unit/protocol effects and integration with production world ownership.
