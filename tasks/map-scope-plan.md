# Client MapScope

One Application-owned MapScope is created/destroyed in Transitions. It owns
SpawnState, map job tokens and renderer/terrain sessions. EntitySpawner remains a
stable service for existing consumers; its immutable DBC/model caches survive a
map change, while its bound SpawnState does not. Renderer caches retain models
and textures; instance retirement uses the existing frame-fence mechanism.

Implementation sequence:

1. A common cancellable JobGroup for map and per-GUID requests, with lifetime tests.
2. Move every spawn queue, GUID map and transient decoded result into SpawnState.
   Preserve process caches and remove reset/shutdown clearing lists.
3. Give the map a terrain session and instance ownership while keeping renderer
   asset caches alive. Cancel/join work before releasing the scene.
4. Replace Application cleanup with MapScope reset. Seed presentation once from
   authoritative entities at entry so early notifications are not lost; remove
   the periodic repair scan and game::World ownership token.
5. Run sweep_guard, domain/lifetime tests, renderer build and native stress runs.
   Record actual memory, convergence, load times and sanitizer coverage, including
   any unavailable instrumentation rather than claiming unmeasured metrics.

Cost/risk: stable services need a strict bound-map contract, renderer model IDs
must remain consistent with retained caches, and terrain workers must finish
against their own immutable session rather than the next map's fields.

## Status (2026-10-15)

Steps 1-4 are done; step 5 is done as far as it can be without a server.

### What MapScope is

`include/core/map_scope.hpp`, `src/core/map_scope.cpp`. Application holds
`std::unique_ptr<MapScope> mapScope_`, created in `beginWorldLoading` and destroyed
there (next entry, including a superseded load or a far same-map teleport), in
`performLogoutToLogin` (logout, disconnect, reconnect), in the CHARACTER_SELECTION
backstop and in `shutdown` - all in Transitions. The offline sky probe builds one
for its scene in `initialize` (also Transitions).

Members, in declaration order (released in reverse):

1. `JobGroup jobs_` - every map job, request token and terrain-worker lease.
2. `EntitySpawner::SpawnState spawns_` - bound with `EntitySpawner::bindMap` in the
   constructor, unbound in the destructor. The constructor refuses a spawner that
   is already bound.
3. `rendering::MapRenderSession render_` - the renderers' instance sessions, quest
   marker/footprint contents and the terrain streaming session. Adopted again with
   `adoptRenderers()` once the load has created the renderers (first entry).
4. `WorldLoader::Session load_` - the map's preparation.
5. Per-map client state that lived in Application: `PlayerModel` (was
   `playerCharacterSpawned`, `spawnedPlayerGuid_/AppearanceBytes_/FacialFeatures_`)
   and `TransportRide` (the M2/WMO ride locks and `deckFloorPending_`).

Destructor order: cancel the JobGroup, unbind the spawner, join the preparation
future, stop the terrain workers, wait for the group; then release what refers to
the scene (player follow target, camera's terrain pointer, mount, logical transports,
equipment dirty tracking); then release the instances (GPU handles retire behind
the frame fences; no device wait); finally the spawn state and the group.

At construction the scope asks `EntityController::announceObjectsInView()` to
present every entity already known - units, players (with mounts), game objects
(with state, transports), corpses and other players' equipment - once. This
replaces the 3-second creature resync scan: creates that arrived before the scope
existed (same network burst as LOGIN_VERIFY_WORLD) or that the server will not
resend (far same-map teleport) are presented from the authoritative entities.

### Removed

- `Application::world`, `getWorld()`, `game::World` (`include/game/world.hpp`,
  `src/game/world.cpp`).
- The creature resync scan in `Application::updateInGame`.
- Map-change and logout clearing lists in `beginWorldLoading`/`performLogoutToLogin`
  (wmo clearAll, m2/cr clear, terrain softReset, spawner resetAllState, ...), the
  dead "reconnect to the same map" branch in `WorldEntryCallbackHandler::enterWorld`
  (a login is always a new scope), `WorldEntryCallbackHandler::resetState`
  (replaced by `beginMapEntry`, called when a load begins),
  `WorldLoader::hasBackgroundWork` (realm patch mounting now waits for no scope),
  `Application::npcsSpawned` and `playerClass_` (never read).
- The per-map re-registration of the NPC death/respawn/swing callbacks in
  `prepareWorldPlayer`; AnimationCallbackHandler owns them from setup (it was
  silently replacing its versions every map).

### Also changed

- `PlayerView::detachWorld()`: the camera controller kept the destroyed terrain
  manager and crashed in `getHeightAt` on the next entry (found by the transfer
  fixture).
- `src/main.cpp`: a terminate handler that names the uncaught exception, and on
  Windows SIGABRT/SIGSEGV/SIGFPE write a module+offset stack to stderr and
  `logs/crash-*.log` (the crash above exited with code 3 and no report).
- `tools/render_dependency_check.py`: `core/job_group.hpp` is header-only
  infrastructure over the already allowed `thread_pool.hpp`; the renderer is
  handed tokens. The check's ceiling stays 0.
- `Entity::hasPosition()` so a corpse is presented only where the server placed it.

### Verified

- `ninja -C build-dev` with no errors or warnings, in both
  `WOWEE_LOADING_FRAME_TEST_CLIENT=ON` and `OFF` (left OFF; `wowee.exe` current).
- `ctest --test-dir build-dev -j8`: 253/253 pass (2 skipped by design:
  framexml_compiles, addon_xml_compiles), including sweep_guard (ceilings unchanged),
  job_group (7 cases), session_domain (14 cases, 2 new for announceObjectsInView),
  world_load_progress.
- `scene_render` lamppost scene renders.
- Native fixture (`tools/test_loading_frames.ps1`, profile
  `build-dev/scratch/loading-baseline-config`): close, resize, disconnect, logout,
  transfer, release, entry-spawn, ready (map 34), disconnect during preparation
  (map 34) all PASS. entry-spawn failed before this work ("entry transition
  discarded the first creature spawn") and now passes without the repair scan.
- Scope teardown on map change measured at ~90 ms (Kalimdor, from the fixture
  logs) in the transition frame behind the loading screen: worker join ~1 ms,
  ~76 ms releasing the terrain session and the character/emitter/marker contents
  (not broken down further), ~1 ms WMO floor cache save, ~11 ms WMO/M2 instances.

### Not measured (need a live server)

- Memory after repeated teleports (e.g. 20), predecoded texture/GUID state growth.
- Spawn convergence in crowded zones without the rescan.
- Logout/relog loops under ASan (no sanitizer build exists for this toolchain).
- Real SMSG_NEW_WORLD transfers and far same-map teleports against a server.

### Remaining / known gaps

- Creature movement flags (walking/swimming/flying) are not stored on the entity,
  so a far same-map teleport or an early create presents the model without them
  until the unit's next movement update.
- `SpellHandler::resetAllState` is game-layer session state, not map state; left.
- The renderer model/texture caches are process-wide and unbounded by map; whether
  they need an LRU budget is a separate decision (they were cleared per map before).
- The player model is reloaded per scope (model id 1), which logs "Model ID 1
  already loaded, replacing" on every map change.
- `EntitySpawner::SpawnState::spawnedPlayerGuid_` / `setLocalPlayerGuid` are never
  set (pre-existing dead state).
