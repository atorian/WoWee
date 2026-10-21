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
The user confirmed restoring zone-based linear distance fog on 2026-10-21:
Fog thickness scales its authored start/end, independently of View Distance
and WDL coverage. Preserve the corrected air colour and nearby volumetric fog;
do not add exponential distance extinction or height clearing on top.

Check: verify near detail, the overlap, far terrain and a real landmark; inspect
memory and draw counts. A new distance limit alone is not a completed LOD fix.
