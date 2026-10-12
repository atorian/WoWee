# Background entity streaming

The render thread used to read/parse player models, decode BLPs and compose
appearance/equipment atlases while processing visibility updates. Grouping GPU
uploads reduced fence waits but left these CPU operations in the frame.

Player appearance and equipment now arrive as prepared results from the two
persistent I/O workers. NPC appearance atlases, weapons and local/remote mount
models/textures use background preparation too. Consumers poll futures with a
zero timeout; they never synchronously load a missing prepared texture. GPU
copies share an asynchronous upload batch on the graphics queue. GPU publication
has a frame budget; this budget is supplementary to background CPU preparation.

Player spawn and equipment revisions reject results after cancellation,
replacement or a change of render instance. Teardown joins owned work before
releasing the asset dataset. Abandoned attachment results still drain into the
CPU cache, freeing queue capacity. Existing mounted visuals remain until a
replacement mount is ready.

## Reproduction

From Windows PowerShell in this checkout:

```powershell
& .\diagnostics\build-entity-streaming.ps1
& .\diagnostics\entity-streaming-probe.ps1
& .\diagnostics\entity-streaming-probe.ps1 -Validate
& .\diagnostics\entity-streaming-probe.ps1 -Validate -Expanded
```

The probe runs a separate offline client/config/log, without touching the active
client or contacting a game server. It queues 24 human players of alternating
sex with real chest art and a real weapon from the local ItemDisplayInfo dataset,
renders the resulting scene, records GPU frames and requires queue completion,
all 24 instances and clean shutdown. Missing prepared textures/composites fail
via explicit error messages. The validated run forces the Khronos layer from
`build/validation-runtime`, rather than relying on a best-effort layer request.
Only the exact pre-existing missing Epic overlay JSON loader messages are exempt.

The comparison executable `build/bin/wowee-streaming-batched-baseline.exe`
contains the same fixture and asynchronous upload batches but synchronous player
CPU preparation. It is a temporary diagnostic artifact, not a source build target.
The probe reports time spent inside queue processing and normal-map publication,
not total frame time, wall-clock loading duration or live Dalaran FPS. Asset caches
start empty per process; operating-system disk caches may already be warm.

The build writes a separate executable and response file. It shares compiler
objects with the primary checkout: avoid concurrent builds that overwrite those
objects while LTO is linking.

The optional expanded scenario covers all ten playable races, two horse mounts,
and cancellation/replacement of player requests already in flight. The GPU
check also exposed a scene-continuation render-pass dependency mismatch; both
scene passes now take their dependency from one shared helper so that the same
framebuffer can be used legally.

## Scope

This change targets entity appearance bursts while walking through a crowded
city. Static M2 game objects and transport doodads still have synchronous model
loading paths; their separate streaming work is not measured by this player probe.
Full live movement, network updates and the complete city are outside the offline
fixture.

## Verified results (2026-10-11)

Release executable SHA256: `6bb0e46cbbdea7b6696b01e24e70d6dcb09e2082af1e2646baec0f38e877a19a`. Installed as `build/bin/wowee.exe`;
the previous executable is preserved as
`build/bin/wowee-before-entity-streaming-20261011-184520.exe`.

| Scenario | Players | Ticks | Total main-thread queue time (ms) | p95 per tick (ms) | Maximum (ms) |
|---|---:|---:|---:|---:|---:|
| batched-baseline | 24 | 25 | 435.966 | 60.3037 | 91.8212 |
| batched | 24 | 28 | 28.4071 | 2.5212 | 3.3985 |
| batched-validated | 24 | 28 | 38.1947 | 3.6365 | 4.0674 |
| batched-validated-expanded | 24 | 50 | 137.508 | 5.2468 | 10.2332 |

Both final validated scenarios completed without renderer VUIDs or errors. The
expanded scenario attached all 24 weapons, published both mounts and confirmed
that cancellation and replacement discarded stale player results. Normal and
expanded recordings/logs remain in `build/` and `build/bin/logs/`.

The active client process keeps running its previous code until restarted.

## Build cleanup (2026-10-11)

At the user's request, obsolete client variants, the duplicate streaming
executable, compiled test/probe executables, temporary Ninja/response files,
linker object snapshots, temporary test configs and the old portable archive
were removed. The cleanup freed 690.5 MiB across 1,036 files. Exact targets and
the unchanged primary/backup hashes are recorded in
`diagnostics/build-cleanup-20261011.json`.

The installed `build/bin/wowee.exe`, its latest pre-streaming backup, runtime
assets/tools and primary incremental build objects remain. Probe logs and
recordings also remain; recreate the streaming candidate with the build script
when another probe is needed. The temporary baseline comparison exe is gone.
