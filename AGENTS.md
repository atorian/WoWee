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

## Tests test behaviour

A test says what the code does in a situation: given this input or this state,
the game sends this packet, a reader sees this value, the interface is told
this, this is drawn. It must fail when that behaviour is wrong - and pass
whatever the code looks like inside.

- **No tests of constants.** Asserting that an enum value, a table entry, a
  count or a define equals a number (`CHECK(SPELL_MOD_OP_COUNT == 32)`) copies
  the source into the test. It fails only when someone edits the constant, and
  then they edit the test too. If a value must agree with the server or the
  data, test what the value does: feed the packet or the DBC row that carries
  it and check the behaviour that follows.
- **No tautologies.** No test that computes its expected value with the code
  under test, sets a field and reads it back through a plain setter and
  getter, restates the implementation line by line, or checks what the
  compiler already guarantees (types, sizes, that a function exists).
- **Through the front door.** Drive the code the way its callers do - packets
  into the game (`GameHarness`), a scene into the renderer (`scene_render`), a
  call on the public API - and read what its callers read. Not private fields,
  not internal steps that could change while the behaviour stays right.
- **A test for a bug fails first.** Run it against the bug before the fix and
  see it fail; a test that never failed has not shown it can.

## Layers

Dependencies point one way - who may include whom:

```
core/       -> game/, rendering/, collision/, audio/, ui/, pipeline/   (the client: owns and wires everything)
ui/         -> game/, rendering/, collision/, pipeline/
game/       -> pipeline/, network/                         (not rendering/, audio/, ui/, core/application)
rendering/  -> collision/, pipeline/                       (wowee_render: not game/, ui/, audio/, network/)
collision/  -> pipeline/                                   (wowee_collision: nothing above it)
```

- **wowee_render** (`src/rendering` less the client's presentation listed in
  `WOWEE_RENDER_CLIENT_SOURCES`) draws what it is handed and knows nothing of
  the game, the interface, the sound or the network.
  `tools/render_dependency_check.py` enforces it through `sweep_guard`.
- **The collision world** (`src/collision`, `wowee_collision`,
  `collision::CollisionWorld`, `renderer->collision()`) is what is solid:
  terrain heights, building and doodad shapes and where they stand, and every
  floor, wall, ray, sight-line and inside-a-building query. The renderers
  place what they draw in it as they place it in their own lists; nothing asks
  a renderer where the floor is. Every query takes a `collision::Reach` -
  `Reach::everywhere()`, or the player's (`CameraController::collisionReach()`)
  for what is only worth asking near the player. A test drives it with no
  device (`tests/test_collision_world.cpp`);
  `tools/collision_dependency_check.py` keeps it below the renderer.
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
  the renderer and the collision world (`RendererTransportScene`,
  `WorldMovementCollision`).

### Testing graphics without the client

`scene_render` links `wowee_render` and nothing of the game or the interface.
It reads a scene (map, place, models, camera, hour, weather, optional terrain),
draws it through the client's renderer and writes a PNG:

```
ninja -C build-dev scene_render
tools/scene_render.sh tools/scene_render/scenes/lamppost.json build-dev/scratch/lamppost.png
```

A renderer edit rebuilds only the renderer and this; a shader edit only its
`.spv`. Building `scene_render` (or `wowee_shaders` alone) also copies the
built shaders and the rest of `assets/` into `build-dev/bin`, where it loads
them from - build it before every run. Use it before reaching for the client,
and keep it building: nothing in `wowee_render` may need the game to link.

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
  `WardenHandler`, `EntityController`, `AccountDataHandler`,
  `ActionBarHandler`, `PetHandler` — owns its state *and* the opcodes that
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

### A new domain handler

A new handler is built from what it uses, never from `GameHandler&`. The older
handlers take `GameHandler& owner` and reach through `owner_.` for anything -
the socket, another handler's state, the facade; do not copy them.
`AccountDataHandler`, `ActionBarHandler`, `InventoryHandler`, `PetHandler`
and `SpellHandler` are the pattern:

```cpp
AccountDataHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                   core::EventBus& events, std::string cacheRoot);
ActionBarHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                 core::EventBus& events, ActionBarQueries& queries);
InventoryHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                 core::EventBus& events, ChatLog& chat,
                 InventoryQueries& queries, const core::CVarStore& cvars,
                 pipeline::AssetManager* assets);
```

State a domain keeps for the character goes in one struct it assigns whole
when a character is logged in (`InventoryHandler::Held`, `beginCharacter`;
`SpellHandler::CharacterSpellState`),
so a field added later cannot keep the last character's value; there is one
such call per domain, from `GameHandler::selectCharacter`, and no reset for a
particular scenario.

What there is to take:

| Need | Interface | Header |
|---|---|---|
| send to the world server, know whether it can, whether a character is in the world | `PacketSender` | `game/packet_sender.hpp` |
| fire a FrameXML event (`"UPDATE_MACROS"`, ...) | `FrameXmlEvents` | `game/frame_xml_events.hpp` |
| put a line in the chat log ("Received item: ...") | `ChatLog` (ChatHandler is one) | `game/chat_log.hpp` |
| a refusal on the error line ("That bag is full.") | `raiseUiError(bus, frameXml, text)` | `game/ui_errors.hpp` |
| say what happened, hear another domain | `core::EventBus&` | `core/event_bus.hpp`, `game/game_events.hpp` |
| a value from `GameServices` | that field alone (`std::string cacheRoot`, `const EmoteTable&`), not the struct | `game/game_services.hpp` |
| ask something of another part of the game | a small interface of your own, in your header (`ActionBarQueries`) | |

`GameHandler` implements these and hands itself over as each one; the handler
never sees the class.

- A handler does not take `GameHandler&`. `tools/domain_shape_check.py`
  counts the handlers that do and the reaches through `owner_.` in
  `src/game`; `sweep_guard` holds both and they only go down.
- State the handler needs that still lives in `GameHandler` moves to its
  owner first - a new handler if it has none, as the action bar did - and
  `GameHandler` keeps a forwarding facade for the old callers. New code asks
  the owner (`gh.actionBar()`), not the facade (`gh.getActionBar()`).
- Two domains that react to each other talk through the bus, not through
  references to each other: `AccountDataHandler` publishes `MacroDeleted` and
  `ActionBarHandler` empties the buttons that held the macro.
- Built in `GameHandler`'s constructor, registered in `registerDomainOpcodes`,
  reached from outside through an accessor returning a reference
  (`accountData()`, `actionBar()`).
- Tested end to end through `GameHarness`, reading state through the
  handler's accessor.

### Build time

`game_handler.hpp` is included by ~60 translation units. Include the narrow
header you need (`game_events.hpp`, a handler's header, a forward declaration)
rather than `game_handler.hpp`, and keep new types out of it.
