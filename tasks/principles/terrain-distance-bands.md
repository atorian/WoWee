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

User choice (2026-10-22): replace the experimental per-surface march with a
shared physical atmospheric transport model. In the enabled mode, zone fog
ranges calibrate extinction rather than retaining the old piecewise curve.
Anchor the broad atmosphere in world coordinates, integrate attenuation and
in-scattering together in a frustum volume, and include weather in that same
integration. Keep geometric retirement at View Distance separate from optical
density. This supersedes the independent near/far layering above for the new
mode; legacy mode remains available. Validate complete Hyjal and Dalaran scenes,
height stability, density response, boundary matching, and GPU cost.

Visual preference confirmed 2026-10-22: preserve the accepted physical-atmosphere Hyjal view. Dalaran should retain a denser atmosphere than the initial sea-level-based implementation; the user disables height influence there and wants to try Thickness 1.2. Global changes must be compared against both viewpoints. The user selected extending regional profiles: author base height, scale height and density in data, blend them through the existing spatial lighting volumes, and preserve the global Fog Thickness control.

User choice (2026-10-22): M2 distance retirement must not add dithering, opacity or a smooth geometric fade. Draw complete models until their existing cull range; atmosphere alone reduces their contrast with distance. Preserve material alpha and intentional instance fades. Check a distant canopy for periodic holes and compare fog-disabled captures inside the cutoff to ensure geometry does not change before culling.

User choice (2026-10-22): Icecrown should have substantially denser atmosphere, with roughly 40% remaining distant visibility as a visual target, not a global opacity value. Scope this to the zone, preserve Dalaran and Hyjal, and validate a full landscape before/after comparison. The user accepted the 300-yard base / 600-yard scale height / 1.2 density profile after the full-scene comparison; its measured distant detail contrast is 33–34% of the prior view. Preserve this as the Icecrown visual baseline.
