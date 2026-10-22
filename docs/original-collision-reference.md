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

## Remaining acceptance work

The full checklist is in `tasks/collision-original-parity.md`. World facet
gathering/masks/order, mover dimensions from model and server updates,
world-supplied surface traversal, complete trajectories, movement-state transitions,
transports and emitted packets still need their own original-reference
comparisons before the replacement is enabled for players. Passing this
contact matrix is not a claim of full gameplay parity or immunity from bans.
