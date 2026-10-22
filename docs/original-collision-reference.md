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

## Remaining acceptance work

The full checklist is in `tasks/collision-original-parity.md`. World facet
gathering/masks/order, mover dimensions from model and server updates,
world-supplied surface traversal, complete trajectories, movement-state transitions,
transports and emitted packets still need their own original-reference
comparisons before the replacement is enabled for players. Passing this
contact matrix is not a claim of full gameplay parity or immunity from bans.
