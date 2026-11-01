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

## Original gameplay behaviour

- User decision (2026-10-22): graphics may change; gameplay behaviour must
  match the corresponding original client. Movement and collision are a
  compatibility contract, including the movement observed by the server.
- Follow `tasks/principles/original-gameplay-behaviour.md`. Port verified
  original rules and compare against independent original-code execution or
  original-client traces; plausible physics and our own expected outputs do
  not establish equivalence.
- Do not expose an unverified replacement to ordinary gameplay or describe
  it as exact, safe from bans, or complete. The current parity work is tracked
  in `tasks/collision-original-parity.md`.

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

## Game data: what an expansion uses is required

User decision (2026-10-30, widened 2026-10-31). It holds in every layer that
reads game data - `game`, `core`, `ui`, `rendering`, `audio`, `pipeline`.

- Each expansion declares the game data it uses: every DBC table and every
  file of its own in `Data/expansions/<expansion>/` (`dbc_layouts.json`,
  `opcodes.json`, `update_fields.json`). The client reads nothing outside
  that list.
- Data on the running expansion's list that is missing, cannot be read, or
  is narrower than the columns read stops the game: `LOG_FATAL` naming the
  file, the expansion and what needed it, then `std::abort()`. No default
  value, built-in copy, zero, guess or "not loaded" branch in its place.
- The list is checked when the expansion's data is opened, so a broken
  install stops at start, not when a player first opens the window that
  reads the table.
- A table the running expansion does not have is not on its list and is
  never opened: no load, no warning. What needs it is a feature that
  expansion does not have, and the code that offers it asks the expansion
  whether it has the feature - never whether a file happened to load.

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
- **A new test must be able to fail.** After writing one, break the code under
  test (invert a condition, return a constant, drop a write) and see it fail,
  then restore the code. Name the realistic bug it catches; a test that
  catches none is deleted.
- **A context is tested on its own.** Domain and service tests link only their
  context's library; fakes implement its ports and the real `core::EventBus`
  delivers its events. What a packet does end to end is still tested through
  `GameHarness`.
- **What runs on the GPU is tested on the GPU.** User decision (2026-10-31):
  a shader's behaviour is not tested by compiling it, or a copy of the
  pipeline around it, for the CPU - that tests a replica, and misses what the
  GPU does differently (dispatch order, barriers, formats, precision). It is
  an acceptance test: a scene drawn by `scene_render` and the picture checked
  (`tools/scene_render/check_*.py`). Those are slow and run apart from the
  unit tests.
- **A test that needs a GPU is registered with `wowee_add_gpu_test`**, never
  `add_test` (user decision, 2026-11-01). It goes into ctest's `gpu`
  configuration, so a plain `ctest` neither lists nor runs it, and takes one
  of two locks, so no more than two run at once under any `-j`. Run them
  with `ctest -C gpu -L gpu` (`ctest --preset dev-gpu`, `./test.sh --gpu`).

## Layers

Dependencies point one way - who may include whom:

```
core/       -> game/, rendering/, collision/, audio/, ui/, pipeline/   (the client: owns and wires everything)
ui/         -> game/, rendering/, collision/, pipeline/
game/       -> <context>/, pipeline/, network/              (not rendering/, audio/, ui/, core/application)
rendering/  -> collision/, pipeline/                       (wowee_render: not game/, ui/, audio/, network/)
collision/  -> pipeline/                                   (wowee_collision: nothing above it)
<context>/  -> base only                                   (wowee_<context>: see "Game layer architecture")
```

- **Every include points down the layers.** `tools/module_boundary_check.py`
  assigns each file under `src/` and `include/` to a module and allows
  includes only into lower layers; it runs as the `module_boundaries` test.
  The includes that broke this when the check was added are listed in
  `tools/module_boundary_baseline.txt`, and that list only shrinks. Fix a new
  violation by moving code or inverting the dependency through an interface,
  never by adding a baseline line. The one exception is a dependency that
  already existed through another header and is now included directly; say
  so in the commit. After fixing an old one, run the check with `--prune`.

- **wowee_render** (`src/rendering` less the client's presentation listed in
  `WOWEE_RENDER_CLIENT_SOURCES`) draws what it is handed and knows nothing of
  the game, the interface, the sound or the network.
  `tools/render_dependency_check.py` enforces it through `sweep_guard`.
  A pass is a cull and a recording: the cull reads the scene (the instances)
  and copies what the pass needs of each one it keeps into a list; the
  `record*` functions read that list and never `scene_`, so a render thread
  can record from a snapshot while the game thread changes the scene.
  `tools/record_scene_check.py` holds it at zero.
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

User decision (2026-10-31): at most two `scene_render` runs at once. Six in
parallel overloaded the Mac. To compare runs, render two, then two more. The
GPU tests in ctest hold to it on their own (`wowee_add_gpu_test`).

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

### Which executable a test is built into

User decision (2026-11-01). A test that links a large library goes into the
suite for that library, not into an executable of its own. With its debug
info, such an executable weighs as much as the library: every copy is
relinked and rewritten on each change to it (forty GameHarness tests used
to write over 15 GB for one edit in `wowee_game`).

- Through `GameHarness` (`wowee_game`): `test_game_end_to_end`.
- Through `ClientHarness`, or anything else that links the client's archive
  (`wowee_client`): `test_client_end_to_end`.
- Register it with `wowee_add_suite_test(<suite> test_<name>)` in
  `tests/CMakeLists.txt`. The ctest entry keeps the file's name without
  `test_` and runs the suite in a process of its own on that file's cases
  alone (`--filenames-as-tags "[#test_<name>]"`), so entries stay
  independent and run in parallel as before.
- A test that links only its own sources, a context library or a small
  layer library (`wowee_protocol`, `wowee_pipeline`, `wowee_render`) stays
  its own executable through `wowee_add_test`: it is small and links in a
  fraction of a second.
- The files of a suite share one link. Keep a file's helpers in an anonymous
  namespace or `static`, or two files' helpers of the same name collide.
- When another group of tests comes to share a library this large, give it a
  suite of its own (`wowee_add_test_suite`) rather than an executable per
  test.

## Game layer architecture (`src/game`, `include/game`, `<context>/`)

`GameHandler` is being taken apart. Each area of the game becomes a bounded
context that owns its state and rules; the handlers become the protocol that
feeds them; `GameHandler` keeps the session. Write new code the way the target
looks, not the way the old code does.

| Layer | Holds | Lives in |
|---|---|---|
| Domain | an area's state and the rules that change it | `<context>/domain/` |
| Services | use cases, the ports they need, the interface consumers use | `<context>/services/` |
| Protocol | opcodes, decoding, the socket: the handlers | `game/` |
| Presentation | chat lines, Lua/FrameXML, sound, drawing | `core/`, `ui/`, `addons/` |
| Composition root | creates every object and wires implementations to ports | `core/application.cpp` |

### Bounded contexts

- An area such as the friend list or the guild is a context with a top-level
  directory of its own: `include/<context>/domain/`,
  `include/<context>/services/`, the same under `src/`, namespace
  `wowee::<context>`, and its own static library `wowee_<context>` that links
  only `wowee_base`.
- `<context>/domain` includes only `base` and its own headers.
  `<context>/services` includes only `base`, its own domain and its own
  headers. Contexts do not include each other's services; two contexts that
  react to each other talk through the bus.
- **Rich, not anemic.** An aggregate owns its state and exposes
  intention-revealing operations (`applyFriendStatus`, `replaceIgnored`).
  No generic writes such as `update(fields)` or `apply(changes)`.
- **The server is authoritative.** The domain applies what the server
  reported; a request to change something is a service operation that goes
  to the server through a port.
- **Operations return the domain events they caused**, as plain values in
  `<context>/domain/events.hpp`. The domain never publishes, logs to chat or
  calls out; its service publishes them on `core::EventBus`.
- **Wire details stay out.** The domain receives decoded records, never
  packets or opcodes. The protocol may keep its own names for those records
  as aliases, and its headers forward-declare them; only the files that
  decode a record include its definition.
- **A refused request is a named outcome** (`NotAFriend`,
  `ContactNameRequired`), published like any other event. The delivery layer
  (chat, Lua, UI) maps outcomes to what the player sees and never re-checks
  business rules.
- State a context keeps for the character is assigned whole when a character
  is logged in, as the handlers' is (below), so nothing keeps the last
  character's value.

### Services and ports

- A service depends only on ports (abstract interfaces in its context's
  `services/`), its domain and `base`. The ports are implemented outside the
  context: the server gateway by the protocol, a name cache by whoever holds
  it.
- Consumers depend on the interface a service implements (for example
  `contacts::IContacts`), never on the service class or on `GameHandler`.
- The protocol talks to a service through a separate feed interface (for
  example `contacts::IContactsServerFeed`).

### Who owns what

- Each domain handler — `ChatHandler`, `CombatHandler`, `InventoryHandler`,
  `MovementHandler`, `QuestHandler`, `SocialHandler`, `SpellHandler`,
  `WardenHandler`, `WorldHandler`, `AccountDataHandler`,
  `ActionBarHandler`, `PetHandler`, `ReputationHandler`, `RealmTimeHandler`,
  `PlayerHandler`, `QueryCacheHandler` — owns the opcodes of its area. A new opcode goes into that handler's
  `registerOpcodes`, never
  into `GameHandler::registerCoreOpcodes`/`registerRemainingOpcodes`. Domain
  handlers register last and overwrite the table, so a second registration in
  `GameHandler` is dead code.
- Until an area moves into a context, its handler also owns the area's
  state. When it moves, the state and rules go to the context and the handler
  keeps only decoding and sending: it hands decoded records to the context's
  feed and implements the context's gateway.
- `GameHandler` keeps the session: connection, auth, Warden, character list,
  world entry, the packet queue and dispatch, and the frame `update`. Do not
  add state, logic, opcodes or forwarding methods to it. Call a handler or a
  context through its accessor (`gh.actionBar()`). The forwarders that still
  exist go with their area: when it moves, delete them and let the compiler
  list every call site.
- The handlers always exist (built in `GameHandler`'s constructor, never reset).
  No `if (xHandler_)`, `xHandler_ ? a : b` or fallback branches.
- One copy of every piece of state. If the field you need lives in a handler
  or a context, ask its owner; never add a mirror of it to `GameHandler`, and
  do not add new `xxxRef()` accessors — the existing ones are being removed.

### How the parts talk

| Kind | Meaning | Mechanism |
|---|---|---|
| Query | "what is X" | an interface passed in (e.g. through `GameServices`, or a context's port) |
| Command | "do X" | a method on whoever owns X's state |
| Notification | "X happened" | an event on `core::EventBus` |

- A context's events live in `<context>/domain/events.hpp`; the protocol's and
  the session's in `include/game/game_events.hpp`. Plain structs, named for
  what happened (`GhostStateChanged`, `OtherPlayerLevelUp`), carrying the
  values a subscriber needs. A subscriber includes the event's header, not
  `game_handler.hpp`.
- `core::EventBus` delivers synchronously, on the main thread, to every
  subscriber in subscription order. Publish with
  `eventBus().publish(events::X{...})` (delivered before it returns). From
  another thread use `post(...)`; the queue is delivered at the start of
  `Application::update`.
- Subscribers are registered by the composition root. Presentation of an
  event (a chat line, an addon event such as `FRIENDLIST_UPDATE`, a sound)
  lives in a subscriber, never in a service or the domain. A subscriber lives
  in the lowest module that may include everything it needs: in `game` when it
  only needs the game, in `core` when it also reads interface settings or
  FrameXML state.
- Subscribe with `bus.subscribe<events::X>(handler)` and keep the returned
  `EventBus::Subscription` as a member of the object the handler captures; it
  ends the subscription when destroyed. The bus is owned by `Application` and
  outlives every subscriber.
- Do not add new `set*Callback` setters or `std::function` callback slots to
  `GameHandler`. When you touch an existing one, move it to an event. Several
  of them were set from two places and the second silently replaced the
  first.
- FrameXML/Lua events (`fireAddonEvent("UNIT_SPELLCAST_FAILED", ...)`) are the
  addon contract and stay strings; they are a separate channel from the bus.

### A handler is built from what it uses

A handler is built from interfaces, never from `GameHandler&`. Since
2026-10-31 none is: the last, `WorldHandler`, takes `WorldQueries`. Do not
bring one back by reaching through a `GameHandler& owner_` for the socket,
another handler's state or the facade. `AccountDataHandler`,
`ActionBarHandler`, `InventoryHandler`, `PetHandler` and `SpellHandler` are
the pattern:

```cpp
AccountDataHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                   core::EventBus& events, std::string cacheRoot);
ActionBarHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                 core::EventBus& events, ActionBarQueries& queries);
InventoryHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                 core::EventBus& events, ChatLog& chat,
                 InventoryQueries& queries, const core::CVarStore& cvars,
                 const GameData& data);
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
| read a client table (`Spell.dbc`, `TaxiNodes.dbc`, ...) | `const GameData&`: `table(dbc, fields, neededFor)` returns it or stops the game; a `GameHarness` test gives rows with `dbc(...)` | `game/game_data.hpp` |
| ask something of another part of the game | a small interface of your own, in your header (`ActionBarQueries`) | |

`GameHandler` implements these and hands itself over as each one; the handler
never sees the class.

- A handler does not take `GameHandler&`. `tools/domain_shape_check.py`
  counts the handlers that do and the reaches through `owner_.` in
  `src/game`; `sweep_guard` holds both at zero.
- Built in `GameHandler`'s constructor, registered in `registerDomainOpcodes`,
  reached from outside through an accessor returning a reference
  (`accountData()`, `actionBar()`).
- Tested end to end through `GameHarness`, reading state through the
  handler's or the context's accessor.

### Composition root

- `Application` creates services, adapters and the event bus, and passes them
  down through constructors and the `GameServices`, `UIServices` and
  `LuaServices` structs.
- Do not add new uses of `Application::getInstance()`.
- Lifetimes: an object is declared before (and so destroyed after) everything
  that calls into it.

### Build time

`game_handler.hpp` is included by ~60 translation units. Include the narrow
header you need (`game_events.hpp`, a handler's header, a forward declaration)
rather than `game_handler.hpp`, and keep new types out of it.

- Headers include only what their declarations need. Use forward
  declarations for types used by pointer or reference, and keep function
  bodies that need heavy headers in `.cpp` files.
- Prefer small interface headers for consumers over large concrete class
  headers.
- Never speed up the build by reducing debug information or optimisation in
  development builds. Fix the dependency structure instead.

## Dead code

Code the running application does not use is deleted, together with whatever
only it kept alive. Do not maintain, extend or test it.

## Changing behaviour

- A refactoring keeps behaviour unless it says otherwise. When unifying state
  changes what the player or an addon sees, list each change in the commit
  message.
- The Lua and FrameXML API is a public contract: addons depend on it.
  Function names, arguments and return values, and the events fired with
  their arguments and order, stay as they are. A refactoring moves the C++
  behind them and leaves what an addon sees unchanged; a fix that does change
  it is listed in the commit as a behaviour change.
