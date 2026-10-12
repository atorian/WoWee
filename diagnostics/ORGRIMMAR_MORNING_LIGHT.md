# Orgrimmar morning light — 2026-10-12

The supplied WoWee scene has harsh outdoor contrast: bright walls/ground and nearly black shaded characters, palms and mountains. The user selected softer contrast/readable shadows and supplied a WoW HD reference. The reference also has different sky art and a visible clock of 07:55; the WoWee log resolves 08:44. This change addresses light balance, not replacement HD art or exact visual parity.

The active settings have RT off, ordinary shadows enabled, normal mapping 1.2, fog strength 0.5 and mist density 2. Light volumes 239/204 select Durotar profiles 247/212. Their 08:44 ambient is approximately (120,109,123)/255 and direct light (255,202,106)/255. Total-light normalization scales both down by about 0.68, reducing shaded red fill to about 0.32 while direct red remains about 0.68.

`applyDaylightFill` transfers 20% of the authored direct light to ambient outdoors before the existing headroom calculation. Each channel of ambient + diffuse is preserved. The transfer fades smoothly from zero at the horizon to full strength at sun elevation Z=0.2. Interiors bypass it and below-horizon light is unchanged. The same per-frame colors reach terrain, WMO, M2, characters and optional RT.

For this morning profile, shaded red fill rises from about 0.32 to 0.46 while the sun-facing red maximum stays 1.0. This is an intentional art adjustment to the existing Lambert model, not a claim to reproduce the original client's shader.

Builds use the latest fast-session object snapshots, plus isolated application/lighting objects. The final single client uses an offline-only baseline flag to bypass the fill; both runs share a real-asset Orgrimmar fixture with a fixed 08:44 clock, camera and isolated settings. No realm connection is made.

Validation: 33 assertions in eight light cases passed, including exact preservation of sun-facing RGB, increased shaded detail and unchanged night/horizon behavior. Both correct-map GPU runs passed: real Kalimdor geometry, map 1, light volumes 239/204, fixed 08:44, clean shutdown and no Vulkan renderer validation errors. Only the two known missing Epic overlay JSON loader diagnostics are excluded. Baseline/fixed recordings contain 45/44 frames. The static frame is dominated by baked/unlit WMO surfaces: the selected wood ROI rises by only 0.6%, while the selected rock/bridge ROIs remain effectively unchanged. Numerical ROI luminance is saved in `build/orgrimmar-light-comparison.json`. This elevated bridge camera does not reproduce the supplied player/lake viewpoint or validate the shaded death knight/palms. The 40% improvement is the calculated ambient red term for lit receivers, not a measured change across the complete scene. Player-view visual validation remains outstanding.

The first attempt accidentally retained GameHandler map 0; its diagnostic result was rejected and the fixture corrected before these runs. The scenario allows 180 seconds for cold world loading.

Installed the verified client as `build/bin/wowee.exe` once the previous process had closed; the installer retains a timestamped executable backup.
