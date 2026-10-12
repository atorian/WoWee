# Mulgore world entry: shared DBC ID index

The reported client log reaches Kalimdor world entry at 19:58:48 and stops
making main-thread progress after 19:59:01; the socket receive thread continues
until the peer closes at 20:00:41. The process had already exited when this
investigation began, so no live hang stack was available.

`AssetManager` returns shared `DBCFile` objects. The first `findRecordById`
previously cleared and populated a mutable `unordered_map` without any lock.
Background entity/equipment readers can enter it concurrently with each other
or with the local character's main-thread appearance code. The eight-reader
cold-start regression reproduced heap corruption immediately in the old code
(`free(): invalid size`, double free and SIGSEGV).

Binary, CSV and JSON loads now build the index before publishing the completed
table through AssetManager. ID lookup only reads the completed index. No
per-frame lock or synchronous asset-loading fallback was added. Reload requires
exclusive ownership, as it already does for the underlying records.

All 24 DBC cases passed (126 assertions). The regression covers concurrent
first reads, missing IDs, repeated binary reloads and replacement of the index
by CSV/JSON data. Commands and results are
recorded in `build/dbc-race-baseline.log` and `build/dbc-race-fixed.log`.

The offline Mulgore fixture uses the same world-entry loader at server position
(-2917.58, -257.98, 52.9968), a level-one female Tauren and 24 additional Tauren
players with real chest/weapon assets. Addon login events are omitted because
there are no live UNIT field packets. Recording uses the existing inline
command-recording path; the parallel timestamp instrumentation is not tested. It uses separate configuration, APPDATA,
logs and recordings and never authenticates. The original terrain-only fixture
completed all 49 initial tiles; it did not reproduce the live hang on its own.

```powershell
& .\diagnostics\build-mulgore-fix.ps1
& .\diagnostics\mulgore-probe.ps1 -Validate
```

The offline fixture checks world entry, player queue completion, GPU recording
and shutdown. It does not establish live realm packet/update behavior; actual
server entry must be checked after restarting the installed executable.

The first full-scene validation also found that terrain, WMO and M2 shadow
layouts expose their 112-byte push block to both vertex and fragment stages,
but draw commands updated only the vertex stage. All three shadow calls now
include both declared stages. The unchanged character shadow layout remains
vertex-only.

Build isolation: the snapshot build compiles the five changed files into its
own object directory, then links stable copies of all 465 client objects.
It preserves the existing runtime shaders. This avoids overwriting or linking
in-progress RT shader/material work from another chat. The normal shared
Ninja build encountered a GLSL default-argument error in that unrelated work.

## Final verification (2026-10-11)

The final strict Vulkan run loaded all 49 initial Kalimdor tiles, completed
all 24 Tauren appearance/equipment queues, recorded actual Mulgore frames
and exited cleanly. No renderer VUIDs or Lua errors remained in this fixture.
Only the two previously missing Epic overlay JSON loader messages were exempt.
Log: `build/bin/logs/mulgore-validated.log`; recording:
`build/mulgore-validated.mp4`; frame: `build/mulgore-final-check.png`.

Installed executable: `build/bin/wowee.exe`, SHA256
`a62922ad3953ba8595f640810c27c4fa0cfc3e9358c2f716cd75cde8f1f80a63`.
Previous client: `build/bin/wowee-before-mulgore-fix-20261011.exe`, SHA256
`6bb0e46cbbdea7b6696b01e24e70d6dcb09e2082af1e2646baec0f38e877a19a`.
Installation verified byte-identical hashes. Live server entry remains untested.

The temporary linker snapshot is removed after installation; its object
manifest remains in `build/mulgore-object-manifest.json`. The build/link scripts
recreate these copies when needed.
