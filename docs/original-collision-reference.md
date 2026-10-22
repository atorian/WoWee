# Collision reference and branch status

Branch: `codex/original-collision-parity`.

This branch fixes three existing query defects and adds a separately verified
contact primitive and surface traversal phase. **It does not yet make player movement equivalent to the
original client.** These phases are linked into the collision library for
offline testing; CameraController and ordinary movement still use the legacy
solver. There is no new gameplay toggle or silent mixed-solver fallback.

## Fixed query defects

- WMO floor pruning no longer throws away the floor nearest the feet because
  another placement supplied a higher floor. Highest-floor queries also no
  longer discard a distinct surface within 0.05 yards of the previous result.
- M2 floor queries retain all authored candidate triangles until the placement
  transform is considered. A model-space wall rotated into a horizontal deck
  now supplies a floor, including queries at a transport's supplied pose.
- Nonzero moves shorter than 0.01 yards no longer bypass the wall query.

The three public-API regression cases failed before their fixes. The existing
legacy wall response, slope policy and grounding rules are not certified by
these tests. In particular the M2 gentle-push response and model-space wall
classification still require replacement, as recorded in the audit.

## Independent executable reference

`tools/original_collision_reference.py` runs build 12340's actual x86 code at
`0x75f9d0` (`DistanceToMove`) in Unicorn. It maps the PE and explicit synthetic
facets/body inputs into an isolated emulator. It does not start Windows, the
game, an account session or a network connection. The original executable is
provided by the caller and is not distributed here.

Required SHA-256:
`0302a63eda5d63150f6cdb5701c5d15ee26aa3a667a8e6febb688b46707f9b81`.

The inputs include ordered facets (plane followed by three vertices), body
position/radius/height, direction and travel distance. Passing an explicit
position bypasses the original world-query validation dependency. The
reference therefore measures contact resolution on supplied geometry, not
the correctness or availability of the world's geometry. The swimming flag
is unset; callers must resolve the original liquid-volume rules separately.

The implementation in `src/collision/movement_contact.cpp` preserves the
original nine-plane volume, face/facet order, clipping, shallow-overlap rule,
short-distance probe and contact ordering. Binary inspection was essential:
using float for every intermediate, or double for every intermediate, changes
the chosen contact. Explicit stores reproduce the x87 spill boundaries, and
the build disables floating-point reassociation and fused multiply-add for
this translation unit.

Supporting addresses: volume planes `0x75c8f0`, volume vertices `0x75ca80`,
polygon clipping `0x75b710`, triangle extrusion `0x75bc50`, quad extrusion
`0x75be80`, travel calculation `0x75c0b0`, facet clipping `0x75c5a0`, lower-face
traversal `0x75cd70`. The pinned
[community reconstruction](https://github.com/SatyPardus/wowdecomp/blob/b1c71b8bd108823bca952f16c6f83a7ad33b9648/WoW/Source/Object/Movement.cpp)
was a navigation aid; it is not the test oracle.

## Repeating the checks

Build the `test_collision_world` and `movement_contact_probe` targets using the
`dev` preset, with TMP, TEMP and TMPDIR pointing to `build-dev/tmp` on D:.
Run CTest's `collision_world` and `original_collision_contact` tests. Python
is required for the latter and is checked by the existing CMake configuration.

The ordinary regression needs only Python's standard library:

```text
python tools/check_original_collision.py --reference tests/fixtures/original_collision_12340.json --probe <built movement_contact_probe executable>
```

To regenerate from the original, supply `--binary <original.exe>` instead of
`--reference`, and `--write-reference tests/fixtures/original_collision_12340.json`.
The emulator requires `pefile==2024.8.26` and `unicorn==2.1.4`; install dependencies
under `build-dev/scratch` with caching disabled and temporary directories on D:.
Do not generate expected answers with the new C++ implementation.

The checked-in fixture contains 1306 original outputs: walls, floors, ceilings,
overlaps, short and long moves, slope boundaries, different body dimensions,
oblique triangles, edges/corners and translated continent coordinates. Results
compare **binary32 bits**, including contact planes in order and the selected
facet; no coordinate tolerance can excuse different contact selection.

The fixture records x87 control word `0x27f`. Running the same matrix with
`0x37f` also passed. This does not establish the original process's FPU mode
for every execution context or prove equivalence on arbitrary inputs.

Validation on 2026-10-22:

- Linux GCC and Windows MinGW: 1306/1306 contact cases match binary32 bits.
- CollisionWorld: 21 cases / 84 assertions pass on Linux and Windows. The
  three new cases fail on the original branch base (five failed assertions).
- AddressSanitizer and UndefinedBehaviorSanitizer: the contact matrix passes.
  Leak detection was disabled because LeakSanitizer cannot run under this
  sandbox's tracing; this is not a leak-check result.
- `sweep_guard` passes its available static checks. Data/interface and
  framexml_run-dependent checks are skipped in this isolated checkout.

These runs compile the affected sources as offline test executables. A full
client build, ordinary player movement and server traffic have not been
validated by this branch's changes.

## Surface traversal

`src/collision/movement_surface.cpp` implements the supplied-geometry surface
phase at `0x7620f0`, including obstacle redirection, the up/forward/down step
attempt at `0x761b00`, `WillPassObstacle` at `0x7619c0` and its speculative
falling checks. The caller owns `SurfaceMovementState`; the solver keeps no
second persistent state. The control flow was adapted from the same pinned
reconstruction and checked against execution of the original binary.

The reference now creates a player-shaped unit in emulator memory so that the
original unit-control checks, stepping, anchor updates and falling transitions
execute. The facet query box encloses all fixture geometry. No physics routine
is stubbed. A read-only hook records calls to `SetTransport`; the real function
continues executing. Comparing the request is not a test of transport object
lookup, attachment or movement between coordinate frames.

The surface fixture contains 1069 reference calls: headroom, tall and narrow
steps, signed slopes around the walkable boundary, ledges, existing step state,
oblique walls, rotated geometry, continent coordinates, uncontrolled-unit
slope policy, facet owners and consecutive stair traversal at different frame
schedules. It compares binary32 coordinates and simulation fields, flags,
consumed time and transport requests. The first straightforward float port
matched only 185 of the initial 252 cases; preserving x87 register intermediates
and memory spills corrected those mismatches. The shared face extrusion used
by both contact and footprint checks has one numerical implementation.

```text
python tools/check_original_surface.py --reference tests/fixtures/original_surface_12340.json --probe <built movement_surface_probe executable>
```

Regeneration uses `--binary <original.exe>` and `--write-reference <fixture>`.
For a Windows executable launched from WSL, large redirected pipes can time
out: write `probe_input` records to a file on D:, run the native probe with
file redirection, and compare its JSON lines using `--actual <output-file>`.
This changes only test I/O, not the compared inputs or algorithm.

Validation on 2026-10-22: Linux GCC and native Windows MinGW both match all
1069 surface cases. AddressSanitizer and UndefinedBehaviorSanitizer passed all
1069 surface cases; LeakSanitizer remains disabled for the sandbox's
tracing limitation. The contact matrix still passes all 1306 cases.

This API remains a surface phase, not a complete frame simulator. Ordinary
falling, jump/landing events, swimming, flying, hover, pending movement actions,
streaming queries and server-visible packets still require separate validation.
It is not connected to CameraController's player simulation.

## Authored WMO and terrain geometry

The WMO loader now retains validated MOBN nodes and MOBR face references in
an immutable `WmoBspData`, passed through model preparation into `WmoGroupShape`.
It rejects incomplete records, missing face/vertex references and child cycles
with a contextual error. The loader regression failed first: the old loader
lost the BSP and accepted both malformed files. Legacy floor/wall queries
remain separate; retaining the BSP does not certify their participation rules.

`queryWmoBsp` executes the recovered positive-before-negative traversal,
original query/MOPY masks, ordered duplicate suppression and 8192 tested-face
limit. The digest path accepts at most 300 faces and 450 distinct vertices;
its boundary comparisons differ from the uncached path. Both are preserved.
`tools/original_wmo_reference.py` runs the original traversal at `0x7ca920`,
including original digest creation and filtering. The 508-case fixture covers
these paths, boundaries, masks, duplicates, capacity and randomized trees.

`makeMovementFacet` reproduces `World::TriDataToFacetData` (`0x782740`), including
local cross products, original placement transforms, precision boundaries,
owner GUIDs and the upward plane supplied for a degenerate face. It reports the
degenerate result to the geometry owner for logging once per asset.
`queryTerrainFacets` reproduces `CMapChunk::Intersect` (`0x7d8840`), using the
original interleaved grid, holes, four-triangle order, chunk-local query margin
and plane construction. Its world corner is supplied explicitly; the world
collector still needs to provide original chunk placement and availability.

Both normalizers have the original x87 and SSE paths. The original selects SSE
from processor features in `CMap::Initialize`; it is not a visual setting.
**Unicorn's built-in RSQRTSS is not an adequate oracle**: it computes an exact
reciprocal square root, unlike the hardware approximation. The reference uses
`tools/original_cpu_reference.cpp` to execute that one instruction on the host
CPU while all original geometry code continues in the emulator. The only
external routine replaced is `SysMsgPrintf`: the reference records its
output-only degenerate-data diagnostic without calling Windows/UI code.

The portable checked-in fixtures use the x87 path: 1017 transformed-geometry
cases and 460 terrain queries. SSE comparison must use the same host CPU as
the original, since its approximation is processor-dependent. Generate SSE
reference output with `--sse --cpu-reference <helper-library>`; do not commit
one processor's SSE outputs as a universal golden fixture. Build the helper
from `tools/original_cpu_reference.cpp` as a shared library in `build-dev/scratch`
(`-shared -fPIC` on Linux; `-shared` on native MinGW), with temporaries on D:.

```text
python tools/check_original_wmo.py --reference tests/fixtures/original_wmo_bsp_12340.json --probe <wmo_bsp_probe>
python tools/check_original_facets.py --reference tests/fixtures/original_facets_12340.json --probe <movement_facet_probe>
python tools/check_original_terrain.py --reference tests/fixtures/original_terrain_12340.json --probe <terrain_facet_probe>
```

Use `--binary <original.exe>` and `--write-reference <output>` to regenerate.
For SSE add the options above. Validation on 2026-10-22: native Windows CMake dev targets built; all nine
selected CTest tests passed (contact, surface, WMO BSP/loading, transformed
facets, terrain, the collision world and existing WMO/interior-light tests).
The changed WMO model preparation also compiled in `wowee_render`. Linux and
native Windows match all 1017 transformed-facet and 460 terrain cases in both
x87 and host-SSE modes. ASan/UBSan passed the BSP, transformed-facet and terrain
matrices; leak detection was disabled for the sandbox tracing limitation.

These are geometry phases: whole-world instance
ordering, M2 collection, liquid/impassable facets and streaming remain separate
acceptance work. The exact geometry queries are not yet the player's source.

## Falling phase

`traceMovementFall` executes the resident-facet Fall phase (`0x7618b0`),
including FallDown (`0x7612b0`), pending move/strafe actions on landing,
fall-time consumption, sliding anchors and the fall-reset timestamp generated
by upward contact. It shares the contact and surface implementation; it is
not yet the player's frame dispatcher. Attached transport transforms, active
splines, world queries and outgoing event delivery remain caller responsibilities.

`tests/fixtures/original_fall_12340.json` contains 2993 calls executed against
the pinned original binary. Coverage includes jumps, terminal and safe falling,
ceilings, slope and edge contacts at rotated/continent coordinates, deferred
input and successive frames under several timing schedules. The first port
failed 323 of the initial 704 calls. Recovering the original x87 spill boundaries
and its nearest-integer elapsed-time conversion removed those differences;
the expanded cases also exposed precision loss in steep-edge normals.
Linux, native MinGW and ASan/UBSan match every recorded binary32 state value
and event. All ten selected native CTest regressions pass.
The existing 1069 surface calls still match. Regenerate with:

```sh
python tools/check_original_fall.py --binary /path/to/wow_original.exe \
  --probe /path/to/movement_surface_probe \
  --write-reference tests/fixtures/original_fall_12340.json
```

## Swimming and flight phase

`traceMovementSwim` reproduces `Swim` (`0x760b40`), which the original also
uses for flight. Solid and liquid facets remain separate. Liquid planes face
into the water and use 75% body height while swimming; after a solid contact,
liquids are no longer consulted during that request. Ascending into the liquid
boundary invokes the original jump restrictions and water-jump velocity.
Attached transport detach and active spline rules are not covered by this API.

The 2848 recorded original calls cover water boundaries, denied water jumps,
gravity flags, wall/ceiling sliding, transformed slopes and multi-frame paths.
Every recorded state value matches on Linux, native MinGW and ASan/UBSan.
All eleven selected native CTest regressions pass; surface and falling fixtures
remain unchanged and pass. `tools/check_original_swim.py` regenerates the
fixture from the pinned binary or compares a probe against the saved reference.

## Hover support and ground normals

`traceMovementHover` reproduces HoverMove (`0x762980`), including the lower-body
support query (`0x75f520`), its controlled/uncontrolled slope thresholds and
transition into Fall when support disappears. The lower-volume contact shares
the verified clipping implementation, without the full body's extra side/top
passes or its minimum probe extension. `queryMovementGroundNormal` reproduces
`0x75ee60`: it clips upward facets to the mover box and averages their normals
in input order before normalization.

The first hover port disagreed with 485 of 2256 original calls; recovering
intermediate precision and spill order removed all differences. A further 1065
original ground-normal queries cover slope thresholds and overlapping facets.
Both matrices match on Linux, native MinGW and ASan/UBSan. All thirteen native
CTest regressions pass, including the unchanged earlier movement fixtures.

## Authored M2 facets

`queryM2Facets` executes the recovered `CM2Model::GetCollisionFacets`
(`0x82ec30`) path: transform vertices, apply world-box outcodes, preserve
triangle order, normalize each placement basis independently and transform the
file's face normals. This is distinct from the WMO cross-product path. The
original consumes a three-float normal per face and does not renormalize the
transformed authored normal.

WoWee previously read these records as four floats and then discarded them at
M2Shape construction. A loader regression failed before the repair (zero normals
instead of two at EOF); the repaired loader-to-shape-to-query test passes all
12 assertions. M2Shape now retains the authored normals, including through
renderer model preparation. Invalid face/index references are diagnosed instead
of being supplied to the new query.

The 1512 original binary calls cover authored normals that differ from cross
products, box boundaries, empty/degenerate geometry, scales and general finite
transforms. They match exactly on Linux, native MinGW and ASan/UBSan. All fifteen
native collision tests pass and both modified WMO/M2 renderer preparation
translation units compile. The new probe targets are registered before the
sanitizer propagation block.

## Remaining acceptance work

The full checklist is in `tasks/collision-original-parity.md`. World facet
gathering/masks/order, mover dimensions from model and server updates,
world-supplied surface traversal, complete trajectories, movement-state transitions,
transports and emitted packets still need their own original-reference
comparisons before the replacement is enabled for players. Passing this
contact matrix is not a claim of full gameplay parity or immunity from bans.

### Terrain boundaries, liquid facets and mover query envelope

`CMap::CreateImpassableFacets` (0x7a43d0) agrees in 1017 original-code
comparisons. It emits two vertical facets per selected horizontal side, in
Y-min/Y-max/X-min/X-max order, using strict comparisons and 32000-yard edges.
There is no extra vertical cull. The new API requires the owner to select
impassable chunks; it does not invent an impassable flag from visual geometry.

`CMap::GetChunkLiquidFacets` (0x7a3cf0), including `CChunkLiquid::GetTris`
(0x7ce960/0x7ce5d0), agrees in 1014 x87 and host-SSE cases. These cover
MH2O cell bits, MCLQ type nibbles, partial grids, cell ordering and Z outcodes.
The API consumes original chunk-local vertices; grid construction, asset
residency and WMO liquids are separate responsibilities still to integrate.

`CMovement_C::GetMoveFacets` (0x75ff90) query preparation agrees in 2500
original-code cases, including grounded/falling/swimming/flying extents,
controlled/local flags, ghosts, water walking and flight-liquid transitions.
The reference observes requests at `World::GetFacets`, supplying a resident
empty world there; it does not replace any query-envelope arithmetic. There
is no attached transport in these fixtures. Numeric spills matter: X centres,
fall extents and expansion radii are retained in register precision where the
original does so, while Y/Z centres are stored as binary32.

`ValidateTestVsFacetQuery` (0x75f0a0) adds another 1200 query-envelope cases,
including cache containment and expansion. `CollideRequestMove` (0x762e00)
now has a service-driven unattached request entry: phase selection, remaining
milliseconds, geometry refresh, liquid plane inversion and GroundNormal are
compared together. The 3413-case matrix includes consecutive fall/jump frames,
slopes, hover, world edges and unavailable initial/repeated queries.

The matrix caught a missing FallDown failure branch: query failure must subtract
skipped milliseconds from anchor/fall counters and call SkipTime before finishing.
It also caught prematurely rounded step-probe normalization, even when final
positions happened to agree. Both regressions failed before correction.

The request fixture supplies world facets at World::GetFacets and observes
CallMoveEventHandlers without executing unit/network delivery. It has no active
spline or attached transport; transport requests are observed, not applied.
This is not an outgoing-packet or production-integration reference. The query
cache is explicit and retained by the dispatcher, so idle/rooted requests can
still evaluate the last collected ground geometry.

### Authored ADT references and flight bounds

ADT loading now retains MCRF MDDF/MODF ordinals in authored order and MFBO's
nine upper/nine lower signed heights. MHDR bit 1 controls participation.
Truncated or out-of-range references and missing required bounds mark collision
metadata invalid and log the failure. The new loader regression failed before
the parser change. Rendering remains available when collision metadata fails.

`CMap::CreateFlightBoundsFacets` at `0x7a4590` matches 1608 original calls,
including signed-16 truncation/wrapping thresholds, sloped bounds, facet order
and planes. Contrary to the earlier working hypothesis, `0x88b9c0` truncates;
it is not nearest-integer FISTP rounding. The routine always uses the x87 plane
constructor `0x7912c0`, irrespective of the facet SSE flag. Native Windows and
ASan/UBSan comparisons pass. These retained bounds still require world wiring.

The native collision/ADT selection passes 22/22 tests. The full sweep remains
at or below its existing ceilings (optional data-dependent checks were skipped).

## WMO model/world and linked objects

The original executable now supplies four further fixture sets:

| Collector | Original entry | Cases | Boundaries |
| --- | --- | ---: | --- |
| WMO model | GetTris `0x7aef00`, GetLiquidFacets `0x7af000` | 682 | Real BSP, liquids, batch/index/face pools and facet conversion |
| World WMO | GetMapObjFacets `0x7a55e0` | 1693 | Real transforms, models and loading barriers; linked service calls recorded |
| Linked doodads | GetDoodadDefFacets `0x7a50c0` | 1248 | Real M2 facets, transformed pending bounds and query-tag state |
| Dynamic entities | QueryDestructibleFacets `0x7a5240` | 1095 | Supplied collision-data callback, real M2 facets and query-tag state |

`tools/check_original_wmo_model.py`, `check_original_wmo_world.py`,
`check_original_doodads.py` and `check_original_entities.py` accept the same
`--binary ... --probe ... --write-reference ...` or `--reference ... --probe ...`
workflow as the earlier tools. WMO model/world checkers also accept `--sse`
with `--cpu-reference` for host RSQRTSS. Portable fixtures use the original
x87 path and retain the executable SHA-256 and FPU control word. Comparison
checks every emitted float's 32-bit representation, facet order and owner.

MOGI group bounds are retained independently from MOGP and visual bounds.
World query centering retains x87 precision for centered extents, then uses
stored float centers for matrix multiplication; box transformation stores each
axis contribution. Missing whole-WMO and missing linked-group branches have
different body-overlap behaviour. No shared "streaming safety" heuristic
replaces them. The model collector preserves both original liquid passes and
the strict pool capacities, including reserved indices for dry liquid cells.

Linked doodads share mutable tags across chunk/group lists. A bounds miss still
marks the object, whereas a disabled, absent or mask-excluded object does not.
Pending preparation marks static flag 0x10000 and uses the transformed shared
model box. Entity collection stops at the first link lacking static flag 2;
its supplied callback may return no collision data, and that still marks the
query tag. Callback parameters and facet ownership are part of the comparison.

All four fixture sets pass on Linux, native MinGW and ASan/UBSan. Both WMO sets
also pass host-SSE comparisons on Linux and native Windows. The native
collision/ADT selection passes 33/33 tests, affected renderer objects compile,
and `sweep_guard` stays within all ceilings (optional unavailable FrameXML
checks skip). These are collection contracts over supplied world state. Real
placement/link lifecycle, combined world collection, the gameplay collision
provider and movement/protocol integration remain unverified; the replacement
is still offline.

## Combined world, dimensions and falling boundaries

`WorldMovementFacets` composes the verified collectors over a supplied
collision-owned view, preserving shared doodad/entity tags, group/chunk order,
partial output and readiness. `original_world_reference.py` runs GetFacets
`0x7a5f20` and its real subordinate collectors: 1072 cases. Entity collision-data
callbacks remain supplied, and fixture links are explicit; this does not verify
production placement/link construction or streaming lifetime.

`movement_dimensions` ports SetCollisionBox `0x6e9570` and SetCollisionHeight
`0x6e9600`. The 1884-case dimension oracle compares their effects through real
GetMoveFacets/cache expansion, including model/server updates in both orders,
forced updates, controlled-player height preservation and reference-scale seams.
It does not substitute immediate application for the pending original event queue.

`original_fall_edges_12340.json` records 3705 original Fall `0x7618b0` executions.
Thin sloped triangles and nearby walls at world coordinates exposed 43 errors
in edge/plane arithmetic. The original keeps some intermediates in x87 register
precision and rounds others at binary32 stores; uniformly using float changed
both nearest-edge selection and support on subsequent contacts. The port now
matches those boundaries in PushOffObstacleEdge `0x75db00`, IntersectPlanes
`0x75d680` (including matrix determinant/adjoint/inverse stores) and the steep
volume test `0x75d740`. Another 75 cases exposed premature FALLING_FAR at the
height boundary in UpdateFallingFar `0x75cf80`; the threshold subtraction must
retain register precision until comparison. Jump-peak calculations in
CheckFallImpactThreshold `0x75e040` preserve it too. Expected states come from
original execution; test geometry is synthetic, not recorded player traffic.

`original_world_movement_reference.py` joins original CollideRequestMove and
GetFacets in a single execution; no facet query is replaced. The 1072 movement
cases compare resultant kinematics, state flags, query sequences, readiness and
observed event-delivery arguments. Original unit/network delivery and the dynamic
entity collision-data provider remain external fixture boundaries. The scene is
shared by both C++ front doors through `tests/world_facet_fixture.hpp`, whose
members own all geometry and cross-list references. Linux and native Windows
comparisons pass; the native collision/ADT selection now passes 37/37 tests.

## Projection and application of frame movement

`movement_plot` ports PlotUnitMovement `0x987b50` and its straight, circular,
spiral and vertical modes. Its 4096 cases compare the complete displacement,
optional facing/pitch writes and returned movement flags. `--cpu-reference`
can execute actual host FSINCOS for the original reference; it agrees with both
the emulator and the port in this matrix. Turn/pitch rates in these fixtures are
finite and positive. Linux, ASan/UBSan and native Windows comparisons pass.

`movement_frame` ports the unattached, uninterpolated ApplyMovement `0x6eac40`
path, including remote GetMoveStartTime `0x6e9f50`, repeated RequestMove calls,
world bounds and anchor elapsed-time updates. The projection's real displacement
is fed through real GetFacets, rather than supplied directly. Both original
thread globals and the empty transport registry are fixture services; unit
notifications are observed. The executable's actual SkipTime clock update and
failed SetTransport path still run. The 1444 cases include six thirty-frame trajectories and pass natively;
552 mismatches in the initial port traced to rounding the anchor target before
subtracting position. RequestMove `0x6e9e20` retains those x87 values despite its
intermediate stores. Counters retain original 32-bit wrap and signed tests.

These APIs are prerequisites for the frame dispatcher. They do not implement
queued player input, interpolation, active splines, attached transports, unit
notification delivery or outgoing packets, and remain disconnected from normal
player control. Synthetic root states follow the original TryRoot-cleared input
invariant; the contradictory root+forward tuple from isolated request tests
loops indefinitely in the original ApplyMovement and is not a frame fixture.

The final 1444-case frame matrix also passes ASan/UBSan. All 39 native collision/ADT
tests and all available sweep ceilings pass for this increment.


## Queued height changes and frame scheduling (2026-10-23)

`MovementCommandQueue` reproduces the height-command portion of
OnSetCollisionHeight/Local (0x6ee7b0/0x6ee760), Enqueue (0x6ec090) and
UpdatePlayerMovement (0x6ef860). Insertion compares signed differences of
wrapping 32-bit clocks and preserves arrival order for equal times. A due
command resets extrapolation delay, changes the collision height, then calls
the unit notification boundary. The notification carries processing time,
not the command's original timestamp. A deferred passenger keeps the command
queued; a future command alone does not report deferral. SetInterpolation
(0x6ea6a0) clears interpolation bits for this local command subset.

`original_commands_12340.json` contains 768 interleaved enqueue/update scenarios,
36,864 update calls. The independent oracle executes the original allocation,
queue insertion, recycling, height setter and dispatch. It observes arguments
and mover state at ProcessLocalMoveEvent (0x7413f0), without asserting that
an actual ACK was sent. Clock rollover, half-range boundaries, equal times,
remote/local notifications, root/transport flags and passenger deferral are
included. Linux and native Windows comparisons and ASan/UBSan pass.

`executeUninterpolatedMovement` adds the ExecuteMovement (0x6f09f0) schedule:
250ms frame cap, SkipTime, slicing at queued commands and heartbeat deadlines,
application of the verified frame solver, due-event processing and deferred
remainder accounting. All 1444 scenarios match bitwise float32 outputs, including
six thirty-frame sequences with height changes, falling/swimming/flight,
turning, long frames and clock rollover. They observe 235 heartbeat requests,
2099 height notifications and 86 retirement requests. Native Windows and
ASan/UBSan pass; the full collision/ADT selection passes 41/41 tests.

The execution oracle runs original ApplyMovement and real world collectors.
Original SendMovementUpdate (0x71f0c0) and UpdateHeartbeatTimerA (0x6e9b70) also
execute, with supplied virtual facing/pitch getters and successful external
packet preparation. Network transmission is observed; packet encoding is not
verified by this fixture. Unit state-event delivery, ProcessLocalMoveEvent,
OnMoveUpdate and the request to RemoveFromMoversList remain explicit external
boundaries. In particular, acknowledging a height command may have additional
unit/network side effects that are not supplied by the observation fixture.

These APIs remain offline. The full command set, unit notification side effects,
packet serialization, interpolation, attached transports/splines and production
world ownership still have to be completed before ordinary gameplay can switch.


## Shared input operations (2026-10-23)

`applyMovementInput` exposes original shared state operations for a mover with
no active spline or attached transport. This reuses the existing SurfaceTraversal
anchor/direction/speed logic instead of introducing another state copy.

Reference addresses: StartMove 0x988a20, StopMove 0x98c8d0, StartStrafe 0x988b00,
StopStrafe 0x98bf80, StartTurn 0x988df0, StopTurn 0x989010, StartPitch 0x989220,
StopPitch 0x989450, StartAscensionDescension 0x9898e0,
StopAscensionDescension 0x989940 and Jump(1) 0x9883f0. The original instructions
execute unmodified; no movement-state operation is replaced by a hook.

`original_input_12340.json` records 5120 cases: 2048 random state combinations
and 3072 explicit mode/input/extra-flag combinations. Checks include airborne
pending direction/stop actions, ordinary start/stop, water/flight ascent,
turning/pitching anchors and jump admission. All float32 state and boolean
return values match on native Windows and ASan/UBSan. The void turn/pitch
starts are explicitly represented by a true adapter result, not interpreted
from an undefined original EAX. All 42 native collision/ADT tests pass; the
expanded input matrix is also rerun natively. Collision dependencies stay
below the renderer/game. The preceding execution increment passed full sweep.

Queue admission and the unit/input/network notification side effects are not
part of this shared-operation API. It remains offline until the complete
original event and world lifecycle is integrated and compared.


## Basic input dispatch and mixed command execution (2026-10-23)

MovementCommandQueue now uses one stable queue for typed height and input
commands. The shared operations above are dispatched with the original
transport/root/pending-root admission rules from UpdatePlayerMovement. The
queue resets movement delay before admission, conditionally emits jump and
ascent notifications, always delivers the original angular-stop boundary, and
requests UpdateHeartbeatTimerA when new movement was not reported by the unit.
The owner applies that reset only for the active mover. Height notification
still occurs after the shape change and its return does not count as `sent`.

Original AddPlayerMoveEvent (0x6ec840), real queue allocation/insertion/recycling,
UpdatePlayerMovement, shared state operations, SetInterpolation and fallback
UpdateHeartbeatTimerA execute in the oracle. Unit ProcessLocalMoveEvent is an
observed external service returning false; angular input stop services are
observed at 0x5f9650 and 0x5fb510. These are explicit fixture boundaries, not
claims that all unit side effects have been implemented.

`original_input_queue_12340.json`: 5120 cases, including timestamp ties and
rollover, rooted/transport/pending-root admission and angular control delivery.
`original_input_execution_12340.json`: 1564 cases with real ApplyMovement/world
collection, including ten thirty-frame sequences, jump/turn/strafe, swimming,
flight, long frames and mixed server height changes. Equal-time height and
input changes preserve their arrival order. Native Windows and ASan/UBSan
comparisons pass. The generalized queue also retains all 36,864 height-only
update results. All 44 native collision/ADT tests pass.

Normal player control remains on the old path. Remaining state-change commands,
unit effects, packet bytes, interpolation, active spline/transport handling
and production world lifecycle are still required before enabling replacement.


## Actual movement status bytes (2026-10-23)

The original WriteMovementStatusToPacket (0x7164b0), client/shared GetMoveStatus
(0x6e90e0/0x987140), CMovementStatus serialization (0x4f4ed0) and CDataStore
writers now execute directly into supplied preallocated storage. No status
field or serializer is hooked. `original_status_12340.json` records 1024
unattached, unsplined mover payloads. Core's
`makeUnattachedMovementStatus12340` creates the corresponding temporary protocol
state from the collision mover; it is still an offline integration adapter.

This comparison exposed two existing protocol defects. The old WotLK writer
omitted pitch when flags2 ALWAYS_ALLOW_PITCHING (0x20) was set without swimming
or flight. It also omitted SPLINE_ELEVATION (0x04000000), while the reader
consumed that float without retaining it. Before fixing the writer/reader,
512/1024 cases failed (384 pitch cases, 256 elevation cases, with overlap).
The writer now emits both conditional fields and MovementInfo preserves the
original elevation value. This fixes the ordinary protocol implementation;
it does not enable the new movement solver.

After the fix, all 1024 encoded payloads match byte for byte. Reading original
payloads and writing the resulting state also preserves every byte and consumes
the complete original input. This is an independent-original-byte test, not a
roundtrip whose expected bytes come from our writer. It checks flags/extra flags,
position, facing, pitch, timestamps, falling velocity/direction and elevation.
The native Windows test, Linux test and ASan/UBSan pass. Existing movement payload
and pre-WotLK packet regressions pass; full sweep remains within all ceilings.

Scope: ordinary movement status, no attached transport or active spline. GUID,
opcode envelopes, ACK counter/value suffixes and the full unit-to-network
notification path remain separate work. No live player/server experiment ran.


## Forced movement ACK envelopes (2026-10-23)

`original_ack_12340.json` records 1280 complete packets from original
CGUnit_C::PrepareMovementPacket (0x71ef80), including GetMoveStatus and actual
serialization, for an active unattached mover without an active spline.
The fixture supplies a preallocated CDataStore and matching active GUID; no
packet writer or field is hooked. Its internal four-byte opcode prefix is
compared separately with Packet::getOpcode; the remaining bytes are the body.

AckMessageNeedsIndex (0x990370) and HasValue (0x7151f0) distinguish the layouts.
Nine speed/rate ACKs, hover, feather fall, water walk, can fly, swim/fly
transition and collision height include a float. Root/unroot, knockback and
gravity ACKs do not. Teleport/worldport have distinct layouts and are excluded.
`check_original_movement_ack.py` compares every byte, varying packed GUID,
counter, movement flags, optional pitch/elevation and falling values.

The production WotLK flag handler omitted the five toggle ACK values.
`test_movement_ack_domain.cpp` reproduced ten failures before the fix, then
passed after the handlers were routed through MovementPacket::buildForceAck.
1280 original envelopes pass natively and with ASan/UBSan. Existing movement
and pre-WotLK layout tests pass. State transitions and packet timing still
need their own integration proof; this is not a full movement parity claim.


## Shared state changes and timed speeds (2026-10-23)

`original_state_12340.json`: 4784 original CMovementShared calls, covering
Change*Speed/Rate (0x989c50..0x98ae90), EnableGravity (0x98b310), FeatherFall
(0x987440), PostFeatherFall/GetFeatherFallVelocity (0x987410/0x986e10),
WalkOnWater (0x9873f0), Hover (0x987460), TryRoot (0x98c4f0) and UnRoot
(0x98c550), with no active spline. Original return values and full shared
state are compared. Speed changes update anchors before recalculating speed;
airborne root becomes pending; feather-fall preserves the separate post-step.

`original_rate_queue_12340.json`: 2304 original enqueue/dispatch comparisons
for nine rate kinds mixed with basic input and height changes. Local/remote
wrappers execute directly, so acknowledgement counters and stable ordering
come from original code. State visible at each rate notification is recorded.
`original_rate_execution_12340.json`: 1444 scheduler/world cases including
six thirty-frame sequences, with state propagated from the original each frame.
The unit delivery boundary reports no send; heartbeat preparation is supplied
as successful. These fixtures establish state and scheduling, not those unit
or network side effects. Native Windows and ASan/UBSan pass all three matrices;
full collision/ADT selection is 49/49 and full sweep passes.


## Unattached knockback (2026-10-23)

Launch (0x6e9ff0), StopSwim (0x98bff0), StopFlying (0x98c240) and local
launch enqueue (0x6ecc80) now execute against independent fixtures. Shared
state coverage grows to 5328 calls. `original_launch_12340.json` compares
2560 standalone launches, including root/swim/flight combinations and raw
non-unit directions. `original_launch_queue_12340.json` compares 2304 mixed
queues; `original_launch_execution_12340.json` compares 1424 full frame/world
scenarios, including four forty-frame sequences.

The original dispatch gate at 0x6ef927..0x6ef965 drops knockback with
ONTRANSPORT before Launch and ACK. Omitting this gate caused 1093 failures;
adding it at dispatch resolves all of them. This does not establish attached
transport semantics: the fixture has no transport GUID or moving passenger
frame. Native Windows and ASan/UBSan match all new matrices; 52 native
collision/ADT tests pass. Unit delivery is still an observed boundary.


## Timed server modes and gravity collision phase (2026-10-23)

The common queue now handles gravity, feather fall, water walk, hover, root,
can-fly and swim/fly transition, preserving ACK order and observed unit/input
boundaries. Shared state now compares 5872 original calls. Mode dispatch matches
2304 mixed queues; complete server-command execution matches 1444 scenes/frames,
including 180 consecutive trajectory frames. Native collision/ADT: 54/54.
The mode queue and execution matrices also pass ASan/UBSan; full sweep passes.

Full execution first exposed 93 mismatches: collision phase and vertical
projection ignored gravity disabled through movement flag 0x400 or flags2 bit 4.
Using the original CanCollideWhileFlying condition resolves all 93.

An additional arbitrary local-input/server-mode sequence remains unresolved:
original_mode_input_boundary_12340.json preserves root + swimming + descending +
disabled gravity on an inactive mover. With unit/input handlers stubbed, original
ApplyMovement repeatedly requests seven milliseconds without consuming time;
the diagnostic reaches its ten-million-instruction budget. This is not evidence
of a complete original-client hang. The diagnostic intentionally fails and is
not part of the passing matrix. Real input admission and unit effects must be
resolved; no invented escape or gameplay rollout was added. Retire observations
are lifecycle requests, not proof of successful removal from the mover list.

References: UpdatePlayerMovement 0x6ef860, GravityStateChanged 0x6eba60,
UpdateHoverState 0x6e9920, HandlePendingActions 0x6eb3b0,
CanCollideWhileFlying 0x75ee00. Unit callbacks are recorded synchronously but
not executed; forced notification still returns false. Fixtures:
original_mode_queue_12340.json and original_mode_execution_12340.json.
The unresolved input case is reproducible with
`tools/probe_original_movement_mode_boundary.py --binary <original WoW.exe>`.
