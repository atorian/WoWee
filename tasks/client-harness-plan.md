# The client's half of transitions, as ordinary tests

Transition bugs live in the client, not in the protocol: the camera held a
pointer to destroyed terrain and crashed on a map change, the first creature of
a login was lost, the interface carried state to the next character.
GameHarness sees only the game. Until now the client half could only be checked
by the native fixture, which needed the whole client rebuilt with
`WOWEE_LOADING_FRAME_TEST_CLIENT=ON` and back again.

## Status (2026-10-16)

Done, built and tested; nothing committed.

- `ctest` has `client_transitions` (`tests/test_client_transitions.cpp`): the
  client's frame and world transitions with no window, GPU, server or special
  build, through `ClientHarness` (`tests/client_harness.hpp`). 9 cases, about
  0.4 s.
- One normal build makes `wowee` and `wowee_loading_test_client` from the same
  client objects; the option is gone. `wowee.exe` has no probe code: no
  `LoadingFrameProbe`, no `Loading probe` messages, no `WOWEE_LOADING_TEST`
  (checked by string search of the binary).

## What was done

### 1. The client as a library, each program its own main

`CMakeLists.txt`:

- `wowee_client_objects` (OBJECT): every client source except `main()`,
  compiled once with the client's flags, PCH and dependencies.
- `wowee_client` (STATIC): the same objects as an archive, for tests - a test
  links only the members it reaches.
- `wowee`: `src/main.cpp` + the icon resource, linked against the objects (not
  the archive: an archive drops members nothing refers to, and the program must
  be the same object set whatever it reaches). Android: the shared library the
  same way.
- `wowee_loading_test_client` (`tests/CMakeLists.txt`):
  `tests/loading_test_client_main.cpp` + `tests/loading_frame_probe.cpp` against
  the same objects. Not a ctest, never installed.
- `framexml_run` and `rt_probe` (both off by default) link the objects instead of
  compiling the client a second time (not configured here - see Risks).
- `main()`'s body moved to `core::runClient(const ClientProgram&)`
  (`include/core/client_main.hpp`, `src/core/client_main.cpp`); `src/main.cpp`
  is three lines. `ClientProgram::observeFrames` is how a fixture's main hands
  the client its observer.

Two dependency faults the split exposed, fixed where they are:

- `src/auth/rc4.cpp` and `vanilla_crypt.cpp` are the world socket's header
  ciphers and now build in `wowee_net`; left in the client, `wowee_net` reached
  up into it and an archive link could not resolve it.
- StormLib's own dependencies are an imported target (`wowee_stormlib`) that
  names them, so they follow the archive on the link line; listed beside it they
  were placed before it once the client was linked as a library (`adler32`
  unresolved).

### 2. No `#ifdef` for tests: a frame observer

`core::FrameObserver` (`include/core/frame_observer.hpp`): `frameBegan`
(Input), `streamingDone` (Streaming), `interfaceStarting` (Interface),
`loopEnded`. `Application::observeFrames` gives one to the loop.
`LoadingFrameProbe` implements it; the nine `#ifdef WOWEE_LOADING_FRAME_TEST_CLIENT`
are gone.

### 3. The frame and the transitions out of Application: `core::ClientLoop`

`include/core/client_loop.hpp`, `src/core/client_loop.cpp`. A move, keeping the
order of every step:

- owns the application state and requested state, the `WorldTransitionQueue`,
  the character's `CharacterSession` (and with it the `MapScope`), the frame's
  phase order (`runFrame`), the interface's input queue and the game's FrameXML
  channel (`setAddonEventCallback` / `setInterfaceCommandCallback`, released in
  its destructor);
- applies the transitions: world entry, the four load steps, logout,
  disconnect, reconnect, exit, the character-screen backstop, the character
  session's start and end, the interface's entry and leave, the reload;
- borrows the game, the event bus, the phase, `PlayerView`, the spawner, the
  world loader and the world entry handler, and is handed three interfaces:

| Interface | What it is | Application | ClientHarness |
|---|---|---|---|
| `ClientWindow` | shouldClose, requestClose, frame delta, pace | `core::Window` + `FramePacer` | a flag and a fixed 1/60 s |
| `ClientInput` | the Input phase: close requests to the window, keys/text/wheel/focus to the interface queue | SDL's queue, pad, touch (the old `pumpInput`) | a queue the test fills |
| `ClientFrontEnd` | 27 hooks at fixed points - login network, world update, scene streaming, load progress, screens, present, render, the map's drawn half (player model, instance floor, portals), music, screens to return to, reconnect, profiling | the old Application code, moved verbatim into the hooks | none of it: no-ops |

The renderer is not an interface: it is a pointer that is null in a client that
draws nothing, as `MapScope` and `CharacterSession` already had it.

To make that true, four borrowed parts now run without a renderer:

- `PlayerView(Camera&, Renderer*, ...)`: the camera controller steers a camera
  it is given (the renderer's in the client). `updateControls` - the camera's
  own update, which turns held movement into movement packets - runs in Present
  for every client; `beforeRendererUpdate` keeps what only a drawn character
  has. `connectMovement(game)` (camera to game: movement, sit, stand,
  auto-follow cancel) moved from `Application::setState(IN_GAME)`, and
  `bindMovementControls(LuaServices&)` from the four closures in `initialize`.
- `WorldLoader`: without a renderer every stage still runs, with nothing to
  read, upload or stand on. It takes the clock its stages are timed by (the
  steady clock by default; the harness's advances one frame per frame) and the
  path of `last_world.cfg` (the same per-user file by default; a scratch file in
  the harness). The doodad upload budget stays on the real clock. Writing
  `last_world.cfg` failing is now said once.
- `WorldEntryCallbackHandler` and `EntitySpawnCallbackHandler` take
  `Renderer*`; without one a same-map teleport is a map entry (there is no
  streamed terrain to keep).
- `CharacterSession::Services::assets` is a pointer; `AnimationController::
  clearMount` no longer dereferences a null audio coordinator.

`core::InterfaceInput` (`include/core/interface_input.hpp`) is the old
`dispatchInterfaceInput`, moved: the binding press/release, the edit box, frame
keys, modifiers, the wheel and the mouse buttons, reached by both the client and
the harness. The development F-keys stay in Application (`keyForClient`).

### 4. The harness and the tests

`tests/client_harness.hpp` / `.cpp`, library `wowee_client_harness`:
GameHarness's game, a real `PlayerView` on its own camera, `EntitySpawner`,
`EntitySpawnCallbackHandler`, `WorldLoader`, `WorldEntryCallbackHandler`, an
`AddonCatalog` on a scratch directory, and `ClientLoop` with the stand-ins.
Every character gets an EventsOnly interface - a real Lua state that hears
events and OnUpdate and loads no FrameXML. `WOWEE_CONFIG_ROOT` points at a
scratch directory, so nothing reaches the player's settings or saved variables.

API: `enterWorld()` (connect, auth, character list, Enter World, load, IN_GAME
- the client's own path), `beginEnteringWorld` / `finishEnteringWorld`,
`enterWorldFromCharacterScreen`, `frame()`, `frameUntil`, `advance`,
`frameUntilHandled`, `closeWindow`, `pressKey` / `releaseKey`,
`receiveNewWorld`, `receiveCreature`, `shutdown`, `runInInterface`,
`interfaceAnswers`; reads `state()`, `session()`, `map()`, `loader()`,
`spawner()`, `camera()`, `running()`, `presentsCreature(guid)`,
`interfaceEvents()` / `interfaceEventsOf(name)`, and the server's records
through `server()`.

The interface's recorder is installed through the harness's own frame
observer at the start of each Interface phase. The loop owns the game's
FrameXML channel, so GameHarness's `addonEvents()` records nothing under
ClientHarness; read `interfaceEvents()`.

Tests (`[client][harness]`), and which were seen failing against the bug
(each fix reverted with the Edit tool, run, restored):

| Case | Seen failing against |
|---|---|
| the real path into the world ends IN_GAME, interface told | - (the harness's own path) |
| closing the window while loading ends the loop before anything else runs; shutdown leaves nothing of the world | yes: the return after Input removed - the server's packet was read and the interface ran |
| a key let go while loading does not leave the character walking | yes: interface input dropped during loading (what the old blocking loader did) - walking again after entry. No: `stopInputMovement` at the load's start removed - the game's NEW_WORLD/teleport/login paths already clear the movement flags and send a stop, so this test cannot tell |
| the first creature of a login is presented once the map is entered | yes: `announceObjectsInView` removed from `MapScope` |
| a map change ends the old map before the next is built, and the character stays | yes: `map_.reset()` removed from `CharacterSession::enterMap` - "EntitySpawner is already bound to another map scope" |
| logout and disconnect, loading or in the world (4 cases), leave nothing | yes: the Interface phase's leave removed - the client never leaves the world |
| the interface answers what it is asked, false as well as true | yes, a bug the harness found: `LuaEngine::evaluateBoolean` ran its chunk for no results and read whatever was on the Lua stack, so every question came back the same. Fixed in `runChunk` (`src/addons/lua_engine.cpp`) |
| the interface goes on hearing the game and running its frames while the world loads | yes, a second bug, found when the native fixture ran against the fix above: a frame made during a load never ran its OnUpdate, because OnUpdate read the visibility the *draw* pass resolves, and nothing is drawn over the world while it loads (nor in a client that draws nothing). Fixed with `WidgetTree::runs(id)`, the shown chain asked of the tree as it stands |
| logging in another character carries none of the last one's interface | yes: the LFG ticked-dungeon accessor put back to a function static |

## What the harness test links

`test_client_transitions` links `wowee_client_harness` -> `wowee_client` (the
archive) + `wowee_game_harness` + the client's libraries. Nothing of the client
is compiled for it - one test source and the harness. The linker pulls the
members reached: the loop, session, map scope, spawner, loader, world entry,
player view and camera controller, the Lua interface and every binding, and
through them most of the client, ImGui and parts of `wowee_render`. The
executable imports SDL3, vulkan-1, the ffmpeg DLLs, unicorn, StormLib's
libraries; it opens no window and makes no Vulkan instance or device - an ImGui
context is created for `ImGui::GetIO()`, which the camera and the input ask.
Other tests are unchanged and do not link the client.

## Native only

- The swapchain rebuilt on a window resize, and the window events SDL sends.
- Real map loading and its timings: terrain streaming, instance geometry, the
  floor cache, upload fences.
- What refers into a drawn scene: the camera's terrain/WMO/M2 collision (the
  map-change crash), the follow target, the mount, a transport registered with
  the renderer. Headless, these are null; the harness covers the order that
  releases them (old scope before new), not the release itself. A transport
  attachment test was tried and dropped: the game itself clears an untracked
  transport on its next movement packet, so it could not fail against the
  `MapScope` fix.
- FrameXML: the harness's interface loads none; a test that needs a binding
  script brings it (as `bindMoveForward` does for MOVEFORWARD).

`tools/test_loading_frames.ps1` runs these through `wowee_loading_test_client`
(no reconfigure). `docs/world-loading-validation.md` says how.

The native fixture was run once (profile `build-dev/scratch/loading-baseline-config`,
default cases) after the evaluateBoolean fix and before the OnUpdate one. Its
first case, `close`, timed out: the world loaded (Kalimdor, 10.6 s) but the
probe never acted, because it waits for "OnUpdate, ordered game events and
posted events delivered during loading" and that check - an evaluateBoolean -
had always come back true by accident. OnUpdate during loading was the false
part; that is the second fix above. The script stops at the first failure, so
the other five cases did not run, and the fixture has not been run again since
the fix: **rerun `tools/test_loading_frames.ps1` before trusting the native
cases.** The loading_frame_probe numbers in
`docs/world-loading-validation.md` from 2026-10-15 were taken with that check
vacuous.

## Remaining

1. `Application::enteredWorld` still registers six `GameHandler` `set*Callback`
   presenters on every world entry (face camera, melee swing, knock-back, camera
   shake, auto-follow, model rebuild); moved verbatim. AGENTS.md wants them on
   the event bus; then the hook goes.
2. The FrameXML channel is still a callback, now owned by ClientLoop. A test of
   the game alone (GameHarness) and one of the client (ClientHarness) cannot both
   own it at once; ClientHarness records what the interface hears instead.
3. Headless presentation of the scene: a test front end with a renderer-free
   scene (counting instances created and released) would let the map change's
   reference release be a unit test, not only its order.
4. The harness's character interface is EventsOnly. A Full interface over a
   checked-in minimal FrameXML would let login events (VARIABLES_LOADED,
   PLAYER_LOGIN) and bindings.xml be exercised too.
5. `requirePhase` is inline and compiled with different fatality in the tests
   (`WOWEE_PHASE_CHECKS_FATAL`) and in the libraries - an ODR difference that
   predates this, more visible now that a test links the whole client.
6. AGENTS.md could name ClientHarness beside GameHarness and scene_render under
   "Testing ...", once this is committed.

## Risks

- `framexml_run` and `rt_probe` are off by default and were not configured
  here: their CMake now links the client objects (and gets LTO at link when the
  objects have it), which should be equivalent but is untested.
- The front end's hooks keep the old order, with one deliberate exception: the
  camera's own update now runs at the start of Present for every client instead
  of inside `presentWorldToRenderer`, i.e. before the offline model probe's M2
  stepping (a fixture with no world) rather than after it.
