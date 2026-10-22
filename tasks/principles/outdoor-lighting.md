# Readable outdoor lighting

Applies when adjusting outdoor lighting in WoWee.

User choice (2026-10-12): morning shadows should retain visible character and foliage detail, with softer contrast. The supplied WoW HD Orgrimmar screenshot is the visual reference for this scene.

Rule: balance directional light and background fill before raising global brightness. Keep sunlit surfaces within the existing color budget and retain night/interior behavior.

Reason: increasing overall brightness makes already bright walls and ground harsher while leaving the light/shade ratio unchanged.

Checklist:
- Check the active graphics settings and visual hour.
- Compare shaded subjects and sunlit surfaces together.
- Verify the real scene at a fixed time; distinguish an improvement from verified visual parity.

User clarification (2026-10-12): avoid strong, sharp character shadows on ground already in shade. Keep only faint soft grounding there; preserve a clear cast silhouette in sunlight, including Sholazar and Stormwind. Derive cast shadows from the actual solar direction and direct-light term, and compare both sunlit and shaded terrain/city floors before accepting a lighting change.

User choice (2026-10-21): leave the animated procedural cloud sheet disabled for now while matching the original clear-sky reference. Retain authored sky-model textures, including their clouds; keep the zone/time palette. Do not restore the procedural layer merely because a weather profile supplies cloud density.

User choice (2026-10-21, supersedes the temporary cloud disable): start with an improved original cloud algorithm. Preserve broad classic shapes, DBC coverage and colours, and authored sky-model replacement rules; add fine detail without introducing a separate cirrus layer or a permanent horizontal sheet. Use one opacity calculation for visible clouds and celestial glare. Check clear Durotar/Barrens, storm cover and an authored Northrend sky before accepting the result.

Cloud iteration workflow (user choice, 2026-10-21): render and evaluate a dedicated offline cloud suite with scene_render. Compare fixed day/night, upward/horizon and clear/overcast captures plus opacity diagnostics. Keep prior outputs for comparison. Do not launch the full game for these iterations; launch it only when requested.

Cloud appearance (user choice, 2026-10-22): use the supplied modern-WoW reference for broad, soft cloud transitions with restrained fine noise. This refines the improved-classic approach; keep DBC colours, weather coverage and the offline iteration workflow.
