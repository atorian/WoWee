# Crystalsong terrain stripes and distant foliage — 2026-10-11

The reported scene is viewed from Dalaran over Crystalsong Forest. The active client was `build/bin/wowee.exe`. The attached screenshot was readable after translating its Windows Temp path to WSL.

## Reproduced cause

MCAL RLE uses the low seven bits as the exact run length. The decoder added one, shifting rows and consuming command bytes as pixels. It also allowed a truncated layer to read the following layer. On `Northrend_31_21.adt`, the wrong count changes 2,139,587 of 2,658,304 compressed alpha texels (80.49%). All 649 correctly decoded streams produce exactly 4,096 texels and consume their layer's exact byte range.

Big alpha maps contain absolute texture coverage, whereas legacy packed maps contain sequential opacity. Applying sequential `mix` to big alpha leaks the base texture into blends and makes neighboring chunks differ when layers are reordered. The mesh, vegetation and dominant-texture queries now share coverage decoding, and the terrain shader sums weighted texture colors. RT chunk albedo uses the same coverage, keeping reflected light consistent with the painted ground. The artificial per-chunk border blur was removed. Texture fetches now remain in uniform layer-count branches, preserving valid mip derivatives.

The real WotLK assets also contain stale MCAL subheader sizes (often 512). Use MCNK `sizeAlpha`, including its eight-byte header, to bound the payload. A canonical header takes priority even if the first pixel bytes spell MCAL. Packed maps honor MCNK `do_not_fix_alpha_map` (0x8000).

Distant trees already skip bone updates, but their vertex shader independently applied synthetic wind every frame. Wind now fades smoothly from 75 to 150 world units, then stays zero. The main and shadow passes use the same fade against the main camera. This stabilizes wind deformation; ordinary draw submission still happens each frame.

Format references: [Noggit RLE decoder](https://raw.githubusercontent.com/wowdev/noggit3/default/src/noggit/alphamap.cpp), [Noggit alpha conversions](https://raw.githubusercontent.com/wowdev/noggit3/default/src/noggit/texture_set.cpp).

## Validation and artifacts

- `diagnostics/terrain-alpha-check.py` independently decodes the extracted tile and writes `build/terrain-alpha-before-after.png`, `build/terrain-alpha-check.json` and a binary reference.
- `diagnostics/terrain-alpha-probe.cpp` runs the real C++ ADT loader and decoder against that reference: all 649 layers / 2,658,304 texels match.
- All four regression targets passed: 610 assertions in 38 cases. Output: `build/terrain-fix-tests.log`.
- Windows client: `build/bin/wowee-terrain-fix.exe`. Build with `diagnostics/build-terrain-fix.ps1`; run regressions with `diagnostics/test-terrain-fix.ps1` and isolated startup with `diagnostics/terrain-smoke.ps1`.
- Final Windows Release executable: 19,243,836 bytes, SHA256 `9c7ff2e6cfa28609f76e6a6db10fd3be14127afa0d25781c14a7e294043c2db7`. Final build output: `build/terrain-fix-build-verified.log`.
- Isolated startup passed: screenshot produced, `Application exited successfully`, no error entries, and terrain/M2/shadow runtime SPIR-V hashes match authored SPIR-V. Output: `build/terrain-smoke-check.log`.
- The comparison image shows coverage with flat diagnostic colors, not an in-game screenshot. Final visual validation still needs this executable in the same reported scene.
