# Loading in the client frame

`Application::runFrame` runs Input → Network → Game → Events → Streaming →
Interface → Present → Render → Transitions → Pace with one immutable frame plan.
`LOADING_WORLD` keeps packets, simulation, spell presentation, posted events and
Lua updates running. Its view builds `LoadingScreen::renderOverlay` in the normal
ImGui frame; Renderer owns the only submission and swapchain resize path.

WorldLoader has no Application pointer or frame loop. Preparation futures read
and parse instance WMO groups, doodads and their textures on I/O workers. Streaming
polls them, advances bounded uploads/collision work and observes tile progress.
Application starts/cleans the scene in Transitions. Warmup is observed after
Present; final upload fences are checked again at the transition boundary.

The former readiness rules are retained:

- Terrain uses initial radius 4, then restores 6. It waits for all unfinished
  tiles, with the previous 60-second timeout and 10-second no-progress fallback.
- Warmup requires two seconds, five consecutive empty spawn-queue observations,
  and WMO floor or terrain within 15 units of spawn height. After five seconds,
  at least four tiles can substitute for the height check. The 25-second hard cap
  remains, with a warning describing incomplete ground/work.
- Pending GPU uploads keep the loading view up instead of blocking in a fence wait.
- Ending a map (`MapScope`) cancels its jobs and joins them in the transition;
  each worker stops at its next token check, so the join is bounded by one read
  or parse. Asset changes wait until no map scope stands; final shutdown ends
  the scope before releasing AssetManager.

Run unit/domain checks with `ctest --test-dir build-dev -R
"world_load_progress|session_domain|lua_handler_globals" --output-on-failure`.

For native integration, use a complete copied profile on D: (no account/server is
contacted by the offline fixture). The scenario lives under `tests/` and is
compiled only in the explicit `WOWEE_LOADING_FRAME_TEST_CLIENT=ON` configuration,
which emits `wowee_loading_test_client` and does not install the client target.
The default `OFF` configuration has no fixture fields, calls, source or environment
switch. Set `TMP`, `TEMP` and `TMPDIR` to `D:\games\WoWee\build-dev\tmp` first:

```powershell
cmake --preset dev -DWOWEE_LOADING_FRAME_TEST_CLIENT=ON
ninja -C build-dev wowee
tools/test_loading_frames.ps1 -ProfileSource D:\path\to\complete-profile
tools/test_loading_frames.ps1 -ProfileSource D:\path\to\complete-profile -Cases ready `
    -MapId 34 -SpawnX 45.7492 -SpawnY 0.6882 -SpawnZ -16.1
tools/test_loading_frames.ps1 -ProfileSource D:\path\to\complete-profile -Cases disconnect `
    -MapId 34 -Stage preparing
cmake --preset dev -DWOWEE_LOADING_FRAME_TEST_CLIENT=OFF
ninja -C build-dev wowee
```

The fixture drives the actual client loop: SDL window close/resize, posted
disconnect/logout/entry notifications, and a virtual movement key pressed and
released while loading. It requires Lua OnUpdate, FIFO game notifications and
posted-event dispatch to work during loading before it performs an action.
Network publication itself is exercised separately by GameHarness.

Each run reports `Loading probe PASS/FAIL`, frame distance and elapsed time, with
logs under `build-dev/scratch/loading-*.out`. Missing data, a phase violation,
wrong destination, a stuck movement key or a timeout fails the script. The profile
is copied in full to the scratch directory; the original profile is not modified.

Local validation on 2026-10-15 (`build-dev`, same Mulgore entry
`map=1, server=(-2917.58,-257.98,52.9968)`):

- Previous blocking loader: 9.123 s; new frame path: 4.127 s (earlier run 4.029 s).
  These are local wall-clock samples, not a cold-cache performance guarantee.
- Close: next Input frame, 8.35 ms. Resize: swapchain matched the new drawable in
  three frames, 34.36 ms.
- Posted disconnect/logout during warmup: login in 165/198 ms. Disconnect during
  unfinished Stockade preparation: 51 ms; no preparation join in the transition.
- Replacement destination entered; a key released while loading caused no motion
  after four seconds in game. Lua OnUpdate, ordered notifications and posted
  events were observed during each scenario's loading frames.
- Stockade entered and rendered in 3.362 s, including a missing floor-cache build.
- Thirteen selected CTest suites, both dependency guards and the standalone
  `scene_render` lamppost scene passed; no phase violations in these native runs.

The native disconnect cases inject the client notification; they do not measure
server/socket failure detection latency. Session-domain tests exercise lifecycle
publication through GameHarness. The restored ordinary client subsequently
received the Isengard Tirion x2 character list and entered Azeroth at
`server=(-8549.58,1003.11,89.7306)` in 7.591 s; the log is
`build-dev/scratch/loading-session-client.out`. Live teleport and socket failure
during loading still need a connected playthrough.

After review fixes, native `sweep_guard` passed (47.26 s), as did the thirteen
selected suites and all six default scenarios through `wowee_loading_test_client`.
Both explicit test and ordinary configurations built successfully. The final
configuration is `WOWEE_LOADING_FRAME_TEST_CLIENT=OFF`: its Ninja graph excludes
the fixture, and `wowee.exe` contains neither `LoadingFrameProbe`, its scenario
messages nor `WOWEE_LOADING_TEST`. Guard/build logs are under
`build-dev/scratch/loading-guard-fixes.*`, `loading-fixture-build.*` and
`loading-shipping-build.*`.
