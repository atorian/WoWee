# Original gameplay behaviour is the compatibility contract

User decision: 2026-10-22. Applies to WoWee gameplay, especially movement,
collision, ground support, jumps, falling, swimming, flight and transports.
Visual improvements are permitted; changing gameplay behaviour is not.

## Rule

Reproduce the matching original client's observable behaviour. A generally
better collision solver, plausible physics, smoother stepping, or passing
our own geometric expectations is not a substitute for compatibility.
For the current movement audit the reference is WoW 3.3.5a build 12340;
do not silently extend that claim to other supported client versions.

Recover and preserve the original geometry participation rules, mover shape
and dimensions, contact and tie-breaking rules, numeric thresholds, stepping,
sliding, support transitions, simulation time handling, and movement reporting.
Do not replace awkward original behaviour with an invented improvement.
Keep visual interpolation separate from simulation state and network packets.

## Reason

Players must not acquire new movement bugs or send movement that differs from
the behaviour expected by their server. A visually acceptable correction can
still change position, jump/fall state or transport offsets on the wire.
Compatibility evidence is required; neither absence of visible bugs nor a
successful login proves it, and neither establishes immunity from a ban.

## Working method

- Pin the reference build and asset version; record binary identity and the
  addresses supporting each recovered rule. Community reconstruction is a
  navigation aid and must be checked against the actual client.
- Establish an independent reference: original-code execution or measured
  original-client traces with recorded inputs, geometry, state and timing.
  Clearly label what has only been inferred from static disassembly.
- Compare both paths on the same inputs. Inspect trajectories, contact/support
  selection, state transitions and emitted movement, not only final position.
- Keep unverified changes in an offline test path until comparison passes;
  do not use ordinary players or their accounts as the initial experiment.
- Do not silently fall back to the old heuristic behaviour when original
  behaviour is unknown. Record the gap and resolve it before claiming parity.

## Acceptance checklist

- [ ] Terrain, WMO, M2, holes, face flags and placement transforms agree.
- [ ] Dimensions, scale and server collision-height changes reach the mover.
- [ ] Walls, corners, ceilings, slopes, stairs, edges and overlapping floors agree.
- [ ] Jump, fall, landing, swimming, flight, knockback and transports agree.
- [ ] Small moves, long frames, contact ties and floating-point boundaries agree.
- [ ] Streaming availability cannot create an unverified movement rule.
- [ ] Packet coordinates, flags, timing, fall data and transport offsets agree
      for matched events, with unrelated session identifiers accounted for.
- [ ] Every remaining divergence is explicit; none is described as full parity.

The diagnostic probes in `docs/collision-audit-2026-10-22.md` demonstrate bugs
in WoWee. Their hand-selected expected positions are not golden results from
the original client and must not become the definition of compatibility.
