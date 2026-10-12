# Sunlight shadows and soft grounding — 2026-10-12

The user clarified the target after the first local-shadow build: avoid strong, sharp silhouettes on ground already in shade; retain visible cast shadows in sunlight, especially Sholazar and Stormwind. The initial fixed-direction map removed 50% of ambient light and created precisely the unwanted extra silhouette. That treatment has been replaced.

## Current change

A separate 1024² depth map resolves nearby characters and M2 cutouts along the same solar rays as the distant shadow map. Its 32-yard projection remains independent of the shadow-distance setting. Terrain and WMO receivers combine local, distant and RT solar visibility with `min`, so the near map improves detail without darkening the same sunlight twice. Detailed silhouettes remove direct light. Closed WMO interiors retain their baked illumination and do not acquire an outdoor solar shadow.

Ambient grounding is separately limited to 6%, with a wider 19 cm filter and a short depth-gap fade of 0.3–1 yard. It cannot create the previous 50% hard body silhouette in existing shade. Self-depth differences below 3–6 cm are rejected and the map footprint edge fades. WMO windows/emissive surfaces and additive local lights keep their previous treatment. Terrain/WMO roofs continue to cast into the main solar map; they do not fill the local map with a second broad roof shadow.

Resources remain separate for both frames in flight and are initialized before descriptor publication. Character/M2 shadow descriptor pools reset only for the first pass. The extra depth images use 8 MiB. The earlier RT startup-layout repair and partial-WMO shutdown cleanup remain in place.

## Validation of the revised treatment

The final Windows Release build passed nine existing lighting/world regressions. Strict real-world Vulkan tests used RT=1, shadow_distance=500 and 8x AA:

| Scene | Coordinates / floor | Check |
| --- | --- | --- |
| Stormwind, 12:00 | -8833, 628 / 93.99 | Before/after on shaded pavement: no strong extra silhouette |
| Stormwind, 12:00, adjacent pavement | -8813, 608 / 95.8113 | Before/after resolves the nearby solar silhouette |
| Stormwind, 17:45 | -8833, 628 / 93.99 | The hard ambient silhouette from the previous build is removed |
| Sholazar, 12:00, camp | 5570.28, 5761.68 / -75.2257 | Before/after with real map 571 and light volume 1690 |
| Sholazar, 12:00, open clearing | 5725, 5925 / -47.89 | Before/after on dry terrain, outside the camp canopy |

Every final run passed renderer validation and clean shutdown. Only the two exact missing Epic overlay JSON loader messages were excluded; no renderer VUIDs were filtered. The first exploratory Sholazar point at 5500,4800 was underwater and was rejected as evidence for sunlit ground. Dry candidates were checked using real ADT heights/object placements and then visually inspected.

`build/refined-shadow-comparison.png` contains unaltered, identically cropped GPU frames: daytime near-map off/on, and previous/revised evening builds. `build/sholazar-solar-comparison.png` contains the clearing comparison. Enlargement uses nearest-neighbor sampling. Test characters are unequipped and the camera differs from the user's captures; this checks lighting behavior, not exact stock-client parity. Foliage animation is not frozen, so isolated changed pixels on leaf edges are not quantitative evidence of shadow strength.

## Reproduction

Run `diagnostics/contact-shadow-probe.ps1 -Scene stormwind|sholazar|brill|dalaran -Variant before|after -Hour 12`. `before` disables the local map only, retaining normal distant/RT shadows. Use `-Hour 17.75` for the evening city check, `-OffsetX 20 -OffsetY -20` for adjacent Stormwind pavement, or `-Scene sholazar -OffsetX 154.72 -OffsetY 163.32` for the dry clearing. `-Legacy` selects legacy render passes (available; the revised validation above used dynamic rendering).

Config, runtime, recordings and logs are isolated under build/. Location/time/offset controls execute only inside the offline probe and do not alter normal gameplay settings or the world clock.
