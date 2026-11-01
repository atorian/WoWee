# Volumetric weather and clear air

Applies when changing fog, light shafts, weather or scattering colours.
The user approved this separation on 2026-10-21 after seeing persistent white
ground fog in Durotar.

Keep weak clear-air scattering available for sun and local-light shafts.
Enabling the volume must not create a dense ground layer in every zone.
Actual fog weather adds low mist; sandstorms add a deeper dust layer with
coloured scattering. Rain and snow may increase suspended particles without
automatically creating ground fog. Do not infer humidity from distance-fog
start/end, manufacture universal dawn mist, or special-case zone names.

Illuminate the medium with the zone's ambient light, sun and local lights.
The distant horizon's matching colour is not ambient illumination. Apply
particle albedo to scattering, including lightning, while extinction remains
the sum of the constituent densities. Capture medium parameters in the owned
frame before rendering; keep the ordinary distance-haze policy independent.

Check: compare clear, fog and sand weather with one density setting; inspect
low and elevated views, coloured light, weather transitions and a held frame.
Log air, mist and dust separately so a white bank has a traceable source.
