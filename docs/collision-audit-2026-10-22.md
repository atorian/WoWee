# Movement collision audit: WoW 3.3.5a and WoWee

2026-10-22. Audit only; no gameplay implementation changed.

Following this audit, the user explicitly required original-client gameplay
behaviour, while permitting graphics changes. The governing requirement is
in `tasks/principles/original-gameplay-behaviour.md`; the implementation and
verification plan is `tasks/collision-original-parity.md`. The redesign below
is subject to that requirement: a generally correct alternative solver is not
an acceptable substitute for demonstrated original-client equivalence.

## Evidence and limits

- WoWee checkout: `424b65d` plus the working tree present during inspection.
  The collision and camera-controller sources inspected were unmodified.
- Original binary: `D:/games/world of warcraft 3.3.5a hd/reserved/wow_original.exe`.
  PE file version **3.3.5.12340**, image base `0x400000`, SHA-256
  `0302a63eda5d63150f6cdb5701c5d15ee26aa3a667a8e6febb688b46707f9b81`.
- Function names and candidate addresses were located using
  [wowdecomp Movement.cpp, pinned revision](https://github.com/SatyPardus/wowdecomp/blob/b1c71b8bd108823bca952f16c6f83a7ad33b9648/WoW/Source/Object/Movement.cpp).
  This is a community reconstruction, not Blizzard source. The important
  call sites, volume construction, step-up sequence and constants were checked
  in the local executable with Capstone. Names are explanatory labels.
- Local binary evidence: `build-dev/scratch/collision-audit/original-disassembly.txt`
  and `disassemble.py`. Downloaded reference files are beside them.
- Behavioural probes compile the actual current `CollisionWorld`, M2 and WMO
  shape implementations and logger with Linux GCC, without a renderer, server,
  or replacement collision implementation. Fixtures use the public API.
  `build-dev/scratch/collision-audit/probe.cpp`, `run.sh`, `results.txt`.
- The original executable was **not run** for movement comparisons. These are
  static binary findings plus executable WoWee reproductions, not a claim of
  matched in-game trajectories or complete reconstruction of the solver.

## What the original does

| Function / address | Verified role |
| --- | --- |
| `GetMoveFacets`, `0x75ff90` | Builds a query volume covering the body and intended movement; expands it differently for ground movement, falling, flight/swimming. Calls the world facet query at `0x783910` (call sites `0x76052b`, `0x760563`). Geometry remains triangles with contact planes and identities. |
| `BuildCollisionVolumePlanes`, `0x75c8f0`; `BuildCollisionVolume`, `0x75ca80` | Builds a convex volume with four upright sides, a top, and four sloping lower faces meeting at the feet: nine vertices, nine planes. The shoulder of the lower pyramid is at `radius * 1.849399`. This is not merely a vertical floor ray or a point with a sideways push. |
| `DistanceToMove`, `0x75f9d0` | Tests the body's requested movement against facets, computes allowed travel and contact planes, and clips movement at contact. Uses the lower pyramid and side/top faces. A small requested distance does not simply authorize crossing geometry. |
| `IsSlopeFallable`, `0x75d340` | Compares a client-controlled mover's facet normal against `0.642787635`, approximately `cos(50 degrees)`, and checks facet support. The comparison uses the signed upward normal. |
| `AttemptStepUp`, `0x761b00` | Probes upward clearance, forward clearance at the raised position, and downward contact. The binary calls `DistanceToMove` at `0x761da9` for up, `0x761e39` for forward and `0x76203c` for down; intermediate obstacle redirection has additional queries. |
| `TraceSurface`, `0x7620f0` | Advances by allowed travel, classifies the contacted surface, tries step-up where appropriate, redirects the remaining movement, and probes downward for support. Losing support leads into falling. |
| `SetCollisionBox`, `0x6e9570` | Stores radius from `width * scale * 0.5`, stores a step budget derived from `scale / referenceScale` (fallback 1), and conditionally updates height. A universal retail step limit of 0.6 is not established by this binary. |

The important distinction is **contact along a legal movement path** versus
**choosing a plausible final floor height**. A surface at a convenient Z does
not establish that the body can reach it, fit below its ceiling, or remain
supported there. Walkability is a movement constraint; an unwalkable surface
must still participate in collision.

## Confirmed WoWee failures

Line numbers refer to the checkout inspected above.

### 1. M2 walls can be walked through — P1, reproduced

`src/collision/collision_world.cpp:1057` and `:1079`:
`m2WallSlide` caps total local push at 0.02 per instance, with individual
corrections limited to 0.015. It tests proximity at the proposed endpoint and
does not clip the move to first contact. Once across a wall, the nearest-point
push can push further onto the new side.

Reproduction: one collision quad at x=0, mover starts at x=-1, attempts one
second at 7 yards/second. Expected to stay at or before x=-0.5. Actual final x:

| Updates per second | Actual x |
| --- | --- |
| 30 | 5.994198 |
| 60 | 5.975267 |
| 144 | 5.914581 |

The same WMO wall stops an ordinary sweep at x=-0.505, providing a control.
The existing test at `tests/test_collision_world.cpp:158` only checks that
one correction points backwards. It does not check that sustained walking
cannot cross the wall.

### 2. Small movements bypass all mover collision — P1, reproduced

`src/collision/collision_world.cpp:1172` returns the requested endpoint when
the squared displacement is at most `1e-4`, i.e. movement of at most 0.01 yard.
That includes real slow movement, not just standing still.

Reproduction: the same WMO wall, x=-1 start, 0.5 yard/second at 60 updates/second
for four seconds. Actual x=0.999999: the mover crosses the wall completely.
The threshold makes collision depend on speed and update frequency.

### 3. Upward movement does not stop the body at a ceiling — P1, reproduced

`src/collision/collision_world.cpp:586`, `:1022`, `:1184`:
the wall paths resolve XY, not a full body's vertical contact. Floor-like
triangles are skipped by wall handling. The sweep cannot reduce upward Z to
stop a jump; for WMO it explicitly keeps `max(candidate.z, adjusted.z)`.

Reproduction: downward-facing ceiling at z=2.2; the two-yard body starts with
feet z=0 and requests feet z=1. It ends at z=1, with its head inside/above the
ceiling, instead of stopping at feet z<=0.2.

`src/rendering/camera_controller.cpp:831` uses this sweep for the local mover.
The following airborne ledge check changes XY only. A separate swimming
ceiling heuristic exists; it does not provide general body/ceiling collision.
This reproduction establishes the collision API failure, not a live jump trace.

### 4. Nearest WMO floor depends on instance insertion order — P1, reproduced

`src/collision/collision_world.cpp:381`:
`if (bestFloor && p.boundsMax.z <= *bestFloor + 0.05f) continue;`
is valid as a highest-floor pruning rule, but remains active when `referenceZ`
requests the nearest floor. A lower instance can be closer to the feet and
still be discarded before triangle testing.

Reproduction: separate WMO instances at z=0 and z=1; query ceiling 1.1,
reference feet 0.1. Adding the higher instance first returns about 1.00003;
adding the lower one first returns about 0.00003. Expected z=0 in both orders.
The tolerance is 0.001 because the floor ray starts 500 yards above the probe.

The existing nearest-floor test uses two floors within **one** WMO instance,
so it misses the cross-instance prune. This can contribute to incorrect floor
selection in overlapping buildings; no specific live-city incident is assigned
to it without a scene reproduction.

### 5. Rotated M2 geometry loses floors or walls — P1, reproduced

`src/collision/m2_shape.cpp:55` classifies triangles into floor/wall lists by
their model-space normal. `src/collision/collision_world.cpp:933` and `:1051`
query only those lists after transforming the ray/mover into model space.
Transforming a ray correctly does not restore triangles already omitted from
the relevant list.

Reproductions, each with a 90-degree Y rotation:

- A local vertical face becomes a horizontal world floor, but `m2Floor`
  returns no floor.
- A local horizontal floor becomes a world wall, but `sweepMover` crosses it
  from x=-1 to x=1.

The shape must remain conservative for arbitrary placement rotations.
Walkability must be evaluated in the mover's world frame. Also review height
and step limits currently applied in local units while only radius is scaled.

### 6. Step clearance and floor reach disagree — P1, collision-stage reproduction

`src/collision/collision_world.cpp:598`, `:659` ignores WMO faces whose top is
within 1.0 yard, plus short risers up to 1.2. Grounded floor selection uses
0.6 (`include/rendering/movement_limits.hpp:12`), and airborne selection uses
1.2 (`src/rendering/camera_controller.cpp:1390`). There is no corresponding
general upward/forward/downward clearance transaction.

Reproduction: a 0.8-yard vertical WMO barrier beneath a downward-facing ceiling
at z=2.1 is traversed from x=-1 to x=1 with feet at z=0. A two-yard body has
only 0.1 yard of upward clearance, so no legal step over this barrier exists.
This probe exercises the collision stage; it does not invoke CameraController's
later grounding. A thin barrier can have no floor under the endpoint for
grounding to recover.

The original has a per-mover step budget and checks the entire attempted path.
Changing all constants to 0.6 or 1.0 alone does not implement that behaviour.

### 7. Terrain slope rejection is explicitly disabled — P1, source-confirmed

`src/rendering/camera_controller.cpp:1009` computes a terrain normal and
`:1028` discards it with `(void)nz`. The nearby comment acknowledges that steep
terrain remains climbable. The terrain penetration rescue later raises a mover
back to the heightfield; it does not reject uphill movement or slide down a
steep contact.

The original's 50-degree contact rule is real; the similarly named constant in
our code does not establish that terrain movement enforces it. Do not fix this
by clearing the terrain floor: that removes support without providing a wall
or slide response. This finding was read in the actual caller, not inferred
from the helper tests. No end-to-end slope simulation was run in this audit.

## Additional confirmed design discrepancies

- `src/game/movement_handler.cpp:1232` reads server collision height, logs it
  and echoes it in the acknowledgement, but does not retain or apply it.
  The collision mover instead uses fixed height 2.0 and radius 0.45/0.50.
  Original collision size is per mover. This needs a domain-owned value and a
  presentation/movement consumer, not another direct GameHandler callback.
- `src/rendering/camera_controller.cpp:1473` and `:1523` can replace a measured
  floor with an interpolated height near `lastGroundZ`; `:1940` can declare
  grounded during a no-floor grace interval. These are verified code paths,
  but visible hovering/edge behaviour was not separately reproduced here.
- Ground caching at `camera_controller.cpp:883` is based on displacement and
  vertical state, not geometry revision. Whether a specific removal/movement
  path invalidates it elsewhere needs a lifecycle test before assigning a
  concrete stale-floor incident.
- WMO floor queries consider all ray-hit triangles, while wall handling uses
  MOPY filtering and a fallback for all-detail groups. The original uses query
  masks and authored BSP traversal. Exact mask equivalence remains unverified;
  no change to MOPY interpretation is justified by this audit alone.

## Verification outcome and implementation direction

The final targeted probe reports **9 failing checks out of 11**: three update
rates for M2 penetration, one insertion order, ceiling, two rotations, low
barrier, and slow WMO walking. Ordinary WMO wall contact and the opposite
floor insertion order pass. These are deliberately failing diagnostic cases,
not a run of the project's full test suite. Scratch files stay under
`build-dev/scratch/collision-audit`; temporary compiler files stay in
`build-dev/tmp`. No game restart was needed.

The structural correction is a common movement solver over conservative world
geometry, returning allowed displacement, contacts and support identity:

1. Expose world-space triangle/contact queries covering terrain, WMO and M2;
   keep authored hole/material participation and transforms explicit.
2. Give the mover its actual dimensions and step budget. Sweep its complete
   volume, resolve remaining movement against contact planes, and handle
   ceiling and steep-slope contacts without deleting the surface.
3. Implement step-up as up/forward/down clearance using the same query; derive
   grounding from actual support. Track support lifetime and transport pose.
4. Replace the camera controller's floor arbitration/rescue stack once the
   solver passes wall, corner, ceiling, stairs, slope, edge, rotated geometry,
   transport and streaming-lifetime tests. Keep the camera as a consumer of
   the resulting player pose.

This is an architectural movement feature, not a safe one-line threshold fix.
The main costs are a new collision query/result API, integrating mover state
with the game/client boundary, replacing existing grounding heuristics, and
validating transports and incomplete streaming. The insertion-order prune and
small-motion bypass also have isolated fixes, but fixing them alone would not
repair the overall movement model.
