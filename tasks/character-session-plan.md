# Client CharacterSession

Everything the client holds for one character in the world is owned by one
object, `core::CharacterSession`, created when the character enters the world
and destroyed when they leave it - logout, disconnect, reconnect, return to
character select, exit. Construction builds it, the destructor tears it down,
and nothing per-character is cleared by hand.

Player-visible reason: after switching characters the interface kept the
previous character's choices - collapsed quest groups, ticked LFG dungeons and
folded headers, the auction sort, the cursor, the trainer and trade skill
filters, the calendar's month - because the Lua bindings kept them in
function-local statics that nothing reset when the Lua state was rebuilt.

## Status (2026-10-15)

Done, built and tested; nothing committed.

### What CharacterSession is

`include/core/character_session.hpp`, `src/core/character_session.cpp`.
`Application` holds `std::unique_ptr<CharacterSession> session_` (its last
member). It is created in Transitions by `Application::beginCharacterSession()`
when the first map load after a login begins (`beginWorldLoading`), and replaced
if a different character - or the same one after its interface left the world -
enters. It is destroyed in Transitions by `performLogoutToLogin`, by the
CHARACTER_SELECTION backstop (with a warning) and in `shutdown`. The offline sky
probe builds one with no interface for its scene.

Members, in declaration order (released in reverse):

1. `addons::InterfaceState` - the former statics of the bindings (see below).
   Outlives the Lua state that reads it, and a /reload.
2. `SpellPresentation` - its bus subscriptions and its restoration/aura timers.
3. `PlayerSync` - the client's per-character frame state that lived in
   `Application`: facing-send cooldown and last yaw, idle yawn, auto-attack edge,
   swim-sheath edge, indoor-crossing edge.
4. The built interface - `std::unique_ptr<addons::AddonManager>` (the Lua state,
   FrameXML, the addons) and which pad's key names its Lua state holds.
5. `std::unique_ptr<MapScope>` - the current map. A map change replaces only this
   (`enterMap`, `enterScene`).

Destructor: the map ends first (jobs cancelled and joined, instances released);
then the interface is reset inside an Interface-phase scope - `~AddonManager`
writes the saved variables under the character name it was built with (a const
member, so they can never land in another character's file) and closes the Lua
state; then the process-wide presenters are told the character left
(`PlayerView::resetCombatVisualState`, `AppearanceComposer::setWeaponsSheathed`,
`AnimationCallbackHandler::resetChargeState`); then the remaining members go.

Interface lifecycle, all in the Interface phase:

- `noteMapReady()` (from `completeWorldLoading`) then `enterWorldInterface()`:
  the first entry loads FrameXML and the addons and sends VARIABLES_LOADED,
  UPDATE_CHAT_WINDOWS, PLAYER_LOGIN, PLAYER_ENTERING_WORLD; every later map entry
  sends PLAYER_ENTERING_WORLD.
- `leaveWorldInterface()` sends PLAYER_LEAVING_WORLD once, while the world still
  stands; `processWorldTransitions` waits while `interfaceHoldsWorld()`.
- `reloadInterfaceIfRequested()` serves ReloadUI and this client's /reload
  (`UIServices::requestInterfaceReload`): a new Lua state over the same
  `InterfaceState`.
- Interface kinds: `Full`; `EventsOnly` for the offline loading fixture, whose
  character has no unit fields (replaces the forced `addonsLoaded_ = true`);
  `None` for the sky probe.

The character's name comes from `core::characterIdentityOf`
(`include/core/character_identity.hpp`): the character list first, the name
cache only when the list lacks the guid; an empty name skips the per-character
saved variables rather than guessing a file.

### Addons

- `addons::AddonCatalog` (process-wide): the addons on disk and addons.cfg.
  The character screen's AddOns list reads and toggles it; each session rescans.
- `addons::AddonManager` (per character): built with the catalog, the
  character's `InterfaceState`, the game, `LuaServices` and the character name.
  Saves the saved variables of the addons it actually loaded, load-on-demand
  ones included (they were never saved before).
- Bindings reach state through the Lua state that runs them: the registry key
  `wowee_interface_state`, bound by `LuaEngine(InterfaceState&)`, read by
  `addons::interfaceState(L)` (`include/addons/interface_state_binding.hpp`). An
  unbound state raises a Lua error rather than sharing anything. No pointer
  survives logout: the registry dies with the Lua state.
- `openInterfaceQuestLog` asks the interface for the row through
  `GetQuestLogIndexByID` (new binding), because collapsed groups are the
  interface's state.
- `LuaErrorReport` (owned by `Application`, handed through `LuaServices`): the
  lua_errors.txt tally spans every character and reload of a run.

Removed: `Application::addonManager_`, `mapScope_`, `spellPresentation_`,
`addonsLoaded_`, `worldInterfaceEntryPending_`, `reloadUiPending_`,
`padKeyNamesFor_`, `cameraIndoors_` and the facing/attack/swim fields;
`loadWorldInterface`, `unloadInterface` and the logout clearing list;
`AddonManager::initialize/unloadAll/reload/shutdown/setCharacterName/scanAddons`.

Also fixed on the way:

- `lua_handler_globals.cpp` kept the legacy handler-globals save table as a
  `luaL_ref` number in a file-scope variable; in the next character's Lua state
  that number named an unrelated table and the saved globals were written into
  it. Now a named registry key per state.
- `ui::frameXmlInterfaceClosed()`: when a Lua state closes, the takeover
  module forgets what that interface had loaded, its cursor icon and the cursor
  bridge. An item held at logout used to draw on the next character's cursor.
- `game::CraftRecipe` moved to `include/game/craft_recipe.hpp` (alias kept in
  `GameHandler`), so the interface state can hold trade skill rows without
  `game_handler.hpp`.

## Per-character vs process-wide

Per-character (`addons::InterfaceState`): the cursor (type, id, slot, bag,
money, guild bank split, held slot, repair mode); quest log collapsed groups and
pending abandon; book page; stable selection; trade skill selection, filters,
dropdown picks, level range, collapsed subclasses and the held row list; trainer
selection, filters, search; talent preview points; send-mail money and COD;
equipment-set ignored slots; auction sort, selection, sell slot; LFG ticked
dungeons and folded headers; guild roster filter, sort, selection and the
once-per-character event log request; who sort; selected friend, ignore,
faction, skill, display channel; loot-roll opt-out; calendar viewed month,
draft, context row; chat colour-by-class; battleground selections and scoreboard
filter and sort; taxi route shown; world map continent and zone. Also in the
session: `SpellPresentation`, `PlayerSync`, the pad key names memo.

Process-wide, left so: key bindings (bindings.cfg); chat window layout and the
extra action bar toggles (interface_state.cfg, account-wide, reloaded into every
Lua state); terrain mip (a video setting); the CVar store; log-once diagnostics,
re-entrancy guards, the click button being dispatched, the missing-API report;
the addon catalog and enabled choices; settings, window, renderer caches;
`PlayerView` (see deviations); the GameHandler (the session with the server).

`tools/function_static_check.py` (in sweep_guard) counts mutable
function-local statics: 167 before, 101 after - 16 in the addon bindings and 85
in the interface, all process-wide by intent; both ceilings only go down.

## Decisions and deviations

- **PlayerView and the `*CallbackHandler`s stay process-wide.** PlayerView's
  camera controller carries the player's camera settings (sensitivity, invert,
  stiffness, pivot, distance, shake), applied once by the settings panel's latch
  and on change. EntitySpawner, AppearanceComposer, five callback handlers and
  `UIServices` hold it, and the handlers are registered through GameHandler
  `set*Callback` setters once at startup. Its per-map parts already end with
  `MapScope`; its per-character residue is reset by the session destructor.
- `setAddonEventCallback` / `setInterfaceCommandCallback` stay (FrameXML events
  are a separate channel from the bus); their bodies now route to the session's
  interface while it holds the world.
- `InterfaceState` survives /reload: the interface is reloaded, the character
  is not.
- `Application::getInstance()` in ui/ and addons/ is unchanged at 45: none of
  those calls reached state that moved into the session.

## Verified

- `ninja -C build-dev`: no errors or warnings (`-Werror`), with
  `WOWEE_LOADING_FRAME_TEST_CLIENT` ON and OFF (left OFF; `wowee.exe` has no
  probe strings).
- `ctest --test-dir build-dev -j8`: 254/254 pass, 2 skipped by design
  (framexml_compiles, addon_xml_compiles), sweep_guard included.
- `test_character_session` drives the real LFG bindings in two characters' Lua
  states (relog starts empty, /reload keeps the choices, an unbound state
  refuses), a relog through `GameHarness` (Alice then Bob: the identity and the
  per-character saved variables file are Bob's), and an unnamed character
  writing no per-character file. With the LFG accessors put back to statics the
  relog and refusal cases fail.
- `test_lua_handler_globals`: a second Lua state's registry tables stay
  untouched by the first state's save table; fails against the old code.
- Native fixture (`tools/test_loading_frames.ps1`, profile
  `build-dev/scratch/loading-baseline-config`), final run, all PASS: close
  8.3 ms, resize 28.2 ms, disconnect 142.9 ms, logout 145.6 ms, transfer
  4344 ms, release 6012 ms, entry-spawn 15844 ms, ready on map 34 2025 ms,
  disconnect while preparing map 34 33.7 ms. Session teardown at logout: about
  90 ms (map 80 ms, Lua close 11 ms). An earlier run had resize time out (the
  window stayed 2560x1369 and no resize arrived) and disconnect take 1.25 s
  after a 29 s cold-cache load with 0.67 s in the map's terrain release; both
  passed on every rerun and neither involved session code.

## Not measured

- 10 relogs under ASan: no sanitizer build exists for this toolchain.
- A live-server relog onto another character, real SavedVariables files across
  two characters, /reload in a live client, memory across relogs.

## Remaining work

1. **Move PlayerView and the callback handlers into the session.** First move
   their GameHandler `set*Callback` registrations to `core::EventBus` events
   (AGENTS.md), so per-session subscribers can own them; split the camera
   settings out of `CameraController` into a process-wide object the session's
   camera reads.
2. **ImGui screens that still carry a character over** (nothing resets them):
   `InventoryScreen` held item (A's item can be placed into B's bags), open and
   dirty flags, pending chat link; `WindowManager` barber state (resetting it
   applies A's original appearance to B) and open-window flags; `ChatPanel`
   sent-line history, whisper target buffer, `CastSequenceTracker`, chat
   bubbles; `ToastManager` queues, timers and `lastKnownZone_` (the next
   character's first zone shows as a zone change); `CombatUI` DPS encounter and
   indexes into the chat history and combat log; `SpellbookScreen` drag state;
   `GameScreen` nameplate context menu, last HP, minimap quest cache, rune
   fill, `pendingTradeItem_`; `MapWindow` selected quest. The proper fix is a
   per-character UI state owner held by the session that these panels read.
3. **Game-layer state not reset at logout or character switch**:
   `ChatHandler` chat history, recent lines, joined channels, AFK/DND, last
   whisper sender (/r on B answers A's whisperer); `CombatHandler` combat log;
   `InventoryHandler` mail inbox and new-mail flag; `GameHandler::barberShopOpen_`.
4. The remaining process-wide statics (16 in addons, 85 in ui) would read
   better with owners that say so; the ratchet holds them meanwhile.
