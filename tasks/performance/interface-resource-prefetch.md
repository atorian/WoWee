# Interface resource prefetch verification — 2026-10-22

Native `interface_prefetch_check`, dev build, WotLK assets, AMD Radeon RX 7700 XT,
Vulkan validation enabled. No realm or account required. Each run starts with an
empty UI/GPU resource cache; the OS filesystem cache is not flushed.

The tool discovers window art from original FrameXML/addon sources, waits by
polling the actual background queue, constructs and opens a previously absent
panel using spellbook/character-frame art, and submits its ImGui draw data to Vulkan. It then requests 600 real
icons to exercise descriptor growth beyond the previous 2048-set backend pool.

Latest completed run:

| Measurement | Result |
|---|---:|
| Window texture variants prepared | 1727 |
| Window prefetch elapsed | 3.973 s |
| Largest producer polling call | 1.141 ms |
| First panel layout/draw extraction | 0.202 ms |
| New loads caused by first opening | 0 |
| Additional character-style icon requests completed | 600 / 600 |
| Total ready images | 2327 |
| Unavailable images | 0 |
| Shared GPU-cache charge after the icon stress test | 267.06 MiB |
| Live VMA allocations at shutdown | 0 |

No VUID/validation resource errors. The Vulkan loader reports two pre-existing
missing Epic Games overlay JSON files before device creation. Those messages
are unrelated to resource uploads and are preserved in the raw log.

The corrected fixture confirms eight vertices in two visible image widgets,
and exits successfully after GPU submission. Earlier first-opening measurements
were invalid: the fixture lacked a panel anchor and did not produce draw vertices.
Those numbers must not be used as an opening-performance baseline.

One earlier run of the corrected fixture observed a 73.437 ms producer polling
outlier while a client was also running (opening itself was 0.244 ms). Split
telemetry was added for discovery, upload retirement and publication; the next
run above had no event over 10 ms. The outlier's cause was not established, so
these measurements do not promise an absolute bound on driver/scheduling delays.

Automated checks: 19 selected CTest cases passed (texture cache, queue, original
spellbook/bindings, widget tree and spell-domain tests). The latest queue suite
passed 55 assertions in seven cases: pending disk work, GPU completion/failure,
path aliases, additive images, missing-file suppression, publication without the
old per-frame texture count, priority and source discovery. A source-discovery
regression was observed failing before fixing apostrophes in comments.

Runtime integration starts shared art discovery at character selection. During
world entry it queries the actual spellbook projection, pet spells, equipment,
bags, keyring and account/character macros. Subsequent state updates keep these
icons queued even if their panels remain closed. This phase wiring was reviewed
in code; the native fixture does not authenticate or measure a live world login.

Commands (with TMP/TEMP/TMPDIR on D: as required by AGENTS.md):

```text
ninja -C build-dev wowee framexml_run test_ui_texture_cache interface_prefetch_check
build-dev/bin/interface_prefetch_check.exe D:/games/WoWee/Data/expansions/wotlk
```

Local raw evidence: `build-dev/scratch/ui-preload/tests.log`,
`native-final-utf8.log` and `final.log`.
