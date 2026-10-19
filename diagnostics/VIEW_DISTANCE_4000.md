# View distance 4000 — 2026-10-19

The previous 2400-yard maximum was a client-side clamp, present at the oldest
local history boundary (2026-10-03), not the camera far plane (30000 yards).
The local history does not establish why that original maximum was chosen.

## Change

- One shared range in rendering/view_distance.hpp is used by terrain, WMO and
  M2 renderers, the client renderer, both settings loaders/sliders and the
  farclip CVar range: 400–4000 yards.
- Terrain streaming derives its radius from the selected distance, without
  the previous six-tile cap. At 4000 the load radius is nine and unload radius
  twelve. World entry restores these configured radii after its smaller
  initial loading area; normal frame updates and taxi streaming preserve them.
- The tooltip now distinguishes world geometry from the independent creature
  and scenery limits, and notes that fog can hide otherwise drawn surfaces.
- Existing defaults and preset values are preserved. The active complete
  profile was copied to build-dev/view-distance-profile on D:, with the
  view_distance setting and its saved farclip CVar changed to 4000. Creature and fog limits are unchanged.
- scene_render accepts optional viewDistance, so its public Renderer path can
  be checked without a server or account.

## Validation

- Before the fix, a scene requesting 4000 reported 2400 and load radius six;
  a transport 3000 yards away drew no M2 batches.
- After the fix, the same scene reports 4000 and load radius nine; the ship
  is visible and its M2 draw calls are nonzero. Fog is disabled for this check
  so it tests geometry distance rather than atmospheric visibility.
- Scene/log artifacts: build-dev/scratch/view-distance-{before,after}.{png,log}.
- Dev client and scene_render build successfully. M2 distance, settings loading
  and schema consistency tests pass: 2196 assertions in 38 cases.
- sweep_guard passes on the configured Python 3.14 runtime; checks requiring
  unavailable Data/interface or framexml_run are explicitly skipped.

The nine-tile loading circle contains about 2.2 times as many tile positions
as the six-tile circle. This checks the accepted range and distant geometry;
it does not establish sustained memory use or frame rate for every map.

## Interactive launch

The first copied profile changed settings.cfg only, leaving farclip=2400 in
cvars.cfg. Startup deliberately reapplies saved CVars, which restored 2400.
Both saved representations are now 4000; the second client launch reads the
complete D: profile and preserves that value. This was profile preparation,
not another renderer clamp.

The first Dalaran run also approached the machine's system commit limit
(0.71 GB free, client private memory 11.9 GB) while still at 2400; it was
stopped. The second launch is at the login screen. Full-world sustained
memory use at 4000 has not been validated.
