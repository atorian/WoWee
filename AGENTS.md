# Instructions for coding agents

## Correct solutions, not quick hacks

- Fix the cause, for the whole class of cases, not the one model or creature
  in front of you. When the phoenix's wing trails are missing because creatures
  draw no emitters, the task is creatures drawing their emitters, not the
  phoenix.
- No per-model special cases, magic offsets, name checks, skipped data or a
  field made to mean two things to make one case look right. If the proper fix
  needs a new API, a new owner for some state or a change to the frame order,
  that is the work.
- A larger correct change is preferred over a small one that only hides the
  symptom. When the proper fix is a real feature, lay out the plan and its
  costs first, then build it fully.

## Disks: everything on D:, nothing on C:

- Build only inside `D:\games\WoWee` — the `build-dev` tree (preset `dev`,
  no LTO) for iteration, `build` (Release, LTO) only for a final release
  measurement.
- Never write to drive C:. It is full, and the system and the game need what
  is left of it.
  - Compilers and linkers put their temporaries in `TMP`/`TEMP`/`TMPDIR`,
    which default to `C:\Users\...\AppData\Local\Temp`. Point all three at
    `D:\games\WoWee\build-dev\tmp` (create it if missing) in every shell that
    runs `cmake`, `ninja`, `c++` or the tests.
  - Scratch files, probe scripts, copied logs and sample dumps go under
    `D:\games\WoWee\build-dev\scratch`, not the system temp directory.
  - Launch the game with its working directory in `build-dev\bin` (or
    `build\bin`), so its `logs\` and `cache\` land on D:.

## Building and running

- `ninja -C build-dev wowee` from `D:\games\WoWee`, with `C:\msys64\mingw64\bin`
  first on `PATH` and `PATHEXT` including `.COM;.EXE;.BAT;.CMD`.
- A fresh `build-dev\bin` needs the runtime DLLs copied from `build\bin` and a
  junction `build-dev\bin\Data` -> `D:\games\WoWee\Data`.
- When a change needs the game rebuilt, close any running `wowee.exe`, rebuild
  and relaunch `build-dev\bin\wowee.exe` without asking. A forced close can
  leave the account "already logged in" on the server for about a minute.
- Useful environment for a test run: `WOWEE_FRAME_PROFILE=1` (frame budget and
  scene counters every 10 s), `WOWEE_LOG_LEVEL=info`, `WOWEE_M2_CULL_CHECK=1`
  (compare column culling against the whole-world walk; slow).
- Another session may be working in the same tree. Check `git status` and
  stage only your own hunks when committing.

## Errors are logged, never swallowed

- Every failure is visible in the log at the default level: `LOG_WARNING` or
  `LOG_ERROR`, never only `LOG_INFO`/`LOG_DEBUG`. Say what failed, for which
  id or path, why when it is known, and what the player sees instead ("rider
  drawn unmounted").
- No silent fallback. A missing file, a model that does not parse, a table row
  that is not there, a texture that falls back to white, work deferred because
  the world is in use - each is reported when it happens. A fallback that keeps
  something on screen is fine; doing it without a log line is not.
- Distinguish "not ready yet" from "failed". A function that returns `false`
  for both leaves its caller retrying a failure every frame with no trace. Give
  failure its own result, log it once, and stop retrying.
- Log a failure once per id/path, not once per frame.

## Layers

Dependencies point one way - who may include whom:

```
core/       -> game/, rendering/, audio/, ui/, pipeline/   (the client: owns and wires everything)
ui/         -> game/, rendering/, pipeline/
game/       -> pipeline/, network/                         (not rendering/, audio/, ui/, core/application)
rendering/  -> pipeline/                                   (wowee_render: not game/, ui/, audio/, network/)
```

- **wowee_render** (`src/rendering` less the client's presentation listed in
  `WOWEE_RENDER_CLIENT_SOURCES`) draws what it is handed and knows nothing of
  the game, the interface, the sound or the network.
  `tools/render_dependency_check.py` enforces it through `sweep_guard`.
- **The game** (`src/game`, the `wowee_game` library) is the protocol and the
  state it builds. It does not draw, play sounds or reach the `Application`:
  it publishes what happened on the event bus and is handed what it must ask
  (`GameServices`: the emote table, the movement collision, the CVar store,
  the world connection; `TransportScene` for transport hulls). Which sound,
  toast or voice line answers an event is the client's choice, not the
  game's. `tools/game_dependency_check.py` holds its includes of the
  presentation at zero.
- **core/** is the client: it owns everything, subscribes to the game's events
  and turns them into pictures and sound (`SpellPresentation`, `PlayerView`,
  the `*CallbackHandler`s), and implements the game's query interfaces over
  the renderer (`RendererTransportScene`, `RendererMovementCollision`).

### Testing graphics without the client

`scene_render` links `wowee_render` and nothing of the game or the interface.
It reads a scene (map, place, models, camera, hour, weather, optional terrain),
draws it through the client's renderer and writes a PNG:

```
ninja -C build-dev scene_render
tools/scene_render.sh tools/scene_render/scenes/lamppost.json build-dev/scratch/lamppost.png
```

A renderer edit rebuilds only the renderer and this; shader edits only need
`ninja -C build-dev wowee_shaders`. Use it before reaching for the client, and
keep it building: nothing in `wowee_render` may need the game to link.

### Testing the game without the client

`tests/game_harness.hpp` (`GameHarness`) runs the real `GameHandler` from
`wowee_game` against a server that is the test - no renderer, sound,
interface, server or account. `enterWorld()` logs a character in; `receive`
plays a server packet, `pump()` runs frames until it is handled; `lastSent` /
`sentOpcodes` read what the game sent back, `addonEvents(name)` what it told
FrameXML, and `events()` is the bus to subscribe to.

Write a domain's tests end to end, as `tests/test_chat_domain.cpp` does: a
packet in, the state it builds, what the interface is told, and the command
that reads that state back out. That is where a copy of the state that one
side writes and the other reads shows up; a parser test cannot see it.

## Game layer architecture (`src/game`, `include/game`)

`GameHandler` is being taken apart into domain handlers, on `master`. Write
new code the way the target looks, not the way the old code does.

### Who owns what

- Each domain handler — `ChatHandler`, `CombatHandler`, `InventoryHandler`,
  `MovementHandler`, `QuestHandler`, `SocialHandler`, `SpellHandler`,
  `WardenHandler`, `EntityController` — owns its state *and* the opcodes that
  change it. A new opcode goes into that handler's `registerOpcodes`, never
  into `GameHandler::registerCoreOpcodes`/`registerRemainingOpcodes`. Domain
  handlers register last and overwrite the table, so a second registration in
  `GameHandler` is dead code.
- `GameHandler` keeps the session: connection, auth, Warden, character list,
  world entry, the packet queue and dispatch, and the frame `update`. Its public
  methods that forward to a handler are a facade kept for callers until they
  move; do not add state, logic or opcodes to it.
- The handlers always exist (built in `GameHandler`'s constructor, never reset).
  No `if (xHandler_)`, `xHandler_ ? a : b` or fallback branches.
- One copy of every piece of state. If the field you need lives in a handler,
  ask that handler; never add a mirror of it to `GameHandler`, and do not add
  new `xxxRef()` accessors — the existing ones are being removed.

### How the parts talk

| Kind | Meaning | Mechanism |
|---|---|---|
| Query | "what is X" | an interface passed in (e.g. through `GameServices`) |
| Command | "do X" | a method on whoever owns X's state |
| Notification | "X happened" | an event on `core::EventBus` |

- Events are plain structs in `include/game/game_events.hpp`, named for what
  happened (`GhostStateChanged`, `OtherPlayerLevelUp`), carrying the values a
  subscriber needs. A subscriber includes that header, not `game_handler.hpp`.
- Publish from the game with `eventBus().publish(events::X{...})` (delivered
  before it returns, main thread). From another thread use `post(...)`; the
  queue is delivered at the start of `Application::update`.
- Subscribe with `bus.subscribe<events::X>(handler)` and keep the returned
  `EventBus::Subscription` as a member of the object the handler captures; it
  ends the subscription when destroyed. The bus is owned by `Application` and
  outlives every subscriber.
- Do not add new `set*Callback` setters to `GameHandler`. When you touch an
  existing one, move it to an event. Several of them were set from two places
  and the second silently replaced the first.
- FrameXML/Lua events (`fireAddonEvent("UNIT_SPELLCAST_FAILED", ...)`) are the
  addon contract and stay strings; they are a separate channel from the bus.

### Build time

`game_handler.hpp` is included by ~60 translation units. Include the narrow
header you need (`game_events.hpp`, a handler's header, a forward declaration)
rather than `game_handler.hpp`, and keep new types out of it.
