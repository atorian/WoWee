# Original-client movement and collision parity

Requirement chosen by the user on 2026-10-22: graphics may change; gameplay
behaviour must match the original client. See
[the governing principle](principles/original-gameplay-behaviour.md).

Status: audit complete; behavioural equivalence not established; runtime
replacement not implemented. This plan supersedes the audit's generic solver
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
- [ ] Executable original-reference comparisons.
- [ ] Equivalent collision geometry and mover parameters.
- [ ] Equivalent contact/surface solver.
- [ ] Equivalent full movement trajectories and state transitions.
- [ ] Equivalent server-visible movement.
- [ ] Verified production integration and removal of competing heuristics.
