# Long-distance terrain and objects

Applies when changing view distance, terrain residency or model LOD.

The user moved the ADT-to-WDL overlap start to 3000 yards on 2026-10-21
after finding the original 1500-yard handoff visibly too close. Keep the
500-yard overlap, ending at 3500 yards.
WDL is terrain only, selected by the active map. Keep detailed terrain around
the viewer, blend representations across this band, and use WDL through the
requested View Distance. Keep a streaming unload margin beyond the detailed
radius to avoid churn at boundaries.

Objects retain their own geometry and available LODs; large landmarks must
survive past the terrain boundary. Small props and clutter need shorter ranges.
Use the same atmospheric perspective for every representation and the sky.
The latest explicit choice on 2026-10-21 supersedes the earlier fixed zone-end
policy: complete outdoor haze extinction must coincide with View Distance.
The purpose of increasing visibility is to see farther, with atmosphere leaving
distant elements faint rather than hiding them at the original zone fog end.
Keep zone colours, time/weather blending and the near gradient; Fog thickness
controls the near haze relative to those profiles. Use a smooth distant tail and
converge to the actual directional sky at the horizontal visibility boundary.
Do not stretch the entire gradient to View Distance and erase nearby depth.
Keep volumetric mist and underwater extinction independent; apply the same
outdoor contrast curve to detailed terrain, WDL, water and models. No continent
or landmark exemptions.

Check: verify near detail, the overlap, far terrain and a real landmark; inspect
memory and draw counts. A new distance limit alone is not a completed LOD fix.
