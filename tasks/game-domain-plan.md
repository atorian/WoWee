# Game domains: from GameHandler to Connection / CharacterSession / MapScope

The game layer (`src/game`, `wowee_game`) is still built around `GameHandler`.
The target (AGENTS.md, "A new domain handler") is domain handlers built from
narrow interfaces, each owning its state and its opcodes, talking to the others
through the event bus - and, at the end, no `GameHandler`: a connection, a
character session and a map scope, the same three lifetimes the client side
already has (`core::CharacterSession`, `core::MapScope`).

## Status (2026-10-16), branch `claude/game-domain-scopes-8ce265`

The first five commits are on master (9574660 includes them); after it, on
this branch:

| Commit | Domain |
|---|---|
| `7bd8a43` | `InventoryHandler` built from what it uses; its state moved out of GameHandler |
| `3123f7e` | `PetHandler` extracted (pet, pet bar, stable) |
| `f9b1cc4` | Action bar cooldowns asked of the spell book, not copied onto buttons |
| `45b24a8` | `SpellHandler` built from what it uses; skills, talent modifiers, explored zones, totems, Spell.dbc and skill tables moved in |
| `2d719e3`, `8464b33` | `ChatHandler` built from what it uses; the character's conversation (history, /r, AFK/DND, channels) begins at each login |
| `76f98fb` | `QuestHandler` built from what it uses; the character's journal (log, windows, shared offer, tracked and completed quests) begins at each login |
| `df2ca44` | `SocialHandler`'s state for a character (`SocialCharacterState`) begins at each login |
| `9d0a9ef` | `SocialHandler` built from what it uses; contacts, summons, minimap pings and the outdoor battlefield moved in with their opcodes; `ReputationHandler` extracted |

Counters (`tools/domain_shape_check.py`, pinned in `sweep_guard`):

| | 048bb47 | after Spell (be38854) | after Quest (76f98fb) | now |
|---|---|---|---|---|
| handlers built from `GameHandler&` | 9 | 7 | 5 | 4 |
| reaches through `owner_.` in src/game | 4213 | 2422 | 2058 | 1206 |
| `xxxRef()` accessors in GameHandler | 218 | 137 | 130 | 110 |
| calls to them through `owner_` | 1320 | 654 | 605 | 505 |
| `set*Callback` setters in GameHandler | 65 | 57 | 52 | 50 |

Bugs found on the way, each with a test that failed first:

- Weapon enchant timers and the rest of the held items survived a relog
  (`InventoryHandler::Held`, assigned whole by `beginCharacter`).
- The pet's bar and spells survived a relog onto a petless character.
- A spell already cooling down, dragged to a new button, showed as ready: the
  button kept a copy of the cooldown that only some packet paths wrote.
- "You cannot read this item" and "pet name invalid" were raised twice.
- Totems survived a relog: a warrior saw the shaman's Searing Totem on the bar.
- `pendingGameObjectLootRetries_` was never filled; the retry loop over it in
  `GameHandler::update` was dead and is gone.
- After a relog /r answered whoever had whispered the last character, and its
  history, AFK/DND and channels stayed.
- The three chat refusals (not in party, wrong faction, restricted) were raised
  twice on the error line.
- After a relog the next character was offered the last one's shared quest,
  saw the quest page it had open, and tracked its quests.
- After a relog a duel challenge or a group invite put to the last character
  was offered to the next, and accepting the duel sent the last one's flag.
- After a relog the last character's at-war flags judged hostility and its
  watched faction stayed on the reputation bar; the reputation list was built
  once per session, so the next character saw the first one's, and a faction
  that became visible never joined it (now rebuilt on `FactionListChanged`).
- `gmTicketActive_` was written and never read; gone.

## Interfaces introduced

| Interface | Header | Implemented by | Used by |
|---|---|---|---|
| `PacketSender` (existing) | `game/packet_sender.hpp` | GameHandler | all converted handlers |
| `FrameXmlEvents` (+ `frameXmlListening()`, `fireFrameXmlEvent(name)`) | `game/frame_xml_events.hpp` | GameHandler | all converted handlers |
| `ChatLog` | `game/chat_log.hpp` | ChatHandler | Inventory, Pet, Spell |
| `raiseUiError(bus, frameXml, text)` | `game/ui_errors.hpp` | free function | Inventory, Pet, Spell, GameHandler |
| `InventoryQueries` | `game/inventory_handler.hpp` | GameHandler (forwards) | InventoryHandler |
| `PetQueries` | `game/pet_handler.hpp` | GameHandler | PetHandler |
| `ActionBarQueries` (+ `spellCooldown`, `itemCooldown`) | `game/action_bar_handler.hpp` | GameHandler | ActionBarHandler |
| `SpellQueries` | `game/spell_handler.hpp` | GameHandler | SpellHandler |
| `ChatQueries` | `game/chat_handler.hpp` | GameHandler | ChatHandler |
| `QuestQueries` | `game/quest_handler.hpp` | GameHandler | QuestHandler |
| `SocialQueries` | `game/social_handler.hpp` | GameHandler | SocialHandler |
| `ReputationQueries` | `game/reputation_handler.hpp` | GameHandler (Faction.dbc) | ReputationHandler |

Events added to `game/game_events.hpp`: `CarriedItemsCounted`,
`LootWindowChanged`, `VendorOpened`, `PlayerEquipmentResolved`,
`SpellsUnlearned`, `SpellSuperseded`, `CastAnimationChanged`,
`PlayerMeleeSwing`, `SprintAuraChanged`, `HearthstoneCastStarted`,
`ChargeStarted`, `CharacterSpellsArrived`, `PlayerNameKnown`,
`UnitSaidAloud`, `UnitEmotePlayed`, `QuestObjectiveProgressed`,
`QuestTrackingChanged`, `GossipOpenedOnNpc`, `GossipClosedOnNpc`,
`ReputationChanged`, `FactionListChanged`, `DungeonFinderOpened`. The
callbacks they replaced (loot window, vendor, equipment, spell-cast
animation, melee swing, sprint, hearthstone preload, charge, chat bubble,
emote animation, quest progress, NPC greeting and farewell, reputation
change, open dungeon finder) are gone from GameHandler.

Per-character state is reset by the one login path: each converted domain
assigns its struct whole on `events::CharacterLoggingIn` (Chat's
`Conversation`, Quest's `Journal`) - Inventory, Pet and Spell still do it from
`selectCharacter` (`beginCharacter`, `resetAllState`) and should move to the
event too.

The `*Queries` interfaces are grouped by the owner of each answer (the fight,
the mount, the items, the game object being opened...). They are what is left
of GameHandler as seen from a domain: each group moves to its real owner as
that owner becomes a handler, and the interface shrinks.

## Order of the remaining domains

By reaches and by how much they block the others:

1. **EntityController** (530 reaches; reads `lastPlayerFieldsRef`,
   `movementInfoRef`, `playerTransport*Ref`, the npc callbacks, and writes into
   every other domain - pet stats, skills, inventory fields, explored zones).
   Blocker: the player's update fields (`lastPlayerFields_`) are GameHandler's
   and read by Quest, Inventory, ActionBar; they belong to a `PlayerState`
   owner (fields, level, xp, stats - today ~40 `player*Ref` accessors), which
   should be cut out first and handed to EntityController as the writer.
   Its ten `npc*`/`creature*`/`gameObject*` callbacks become events
   (`UnitSpawned`, `UnitDied`, ...) - client subscribers are
   `EntitySpawnCallbackHandler` and `AnimationCallbackHandler`.
2. **MovementHandler** (368; `movementInfoRef`, transport refs, mount and
   creature-move callbacks, area triggers). Blocker: `movementInfo` is read by
   Spell (through `SpellQueries::playerMovement`), Combat and Chat; it moves
   into MovementHandler with `MovementQueries` for the transport manager.
   The mount state (`currentMountDisplayId_`, `mountAuraSpellId_`) goes with it
   - `SpellQueries`' mount group then points at MovementHandler.
3. **CombatHandler** (291; tab-cycle refs, resurrect refs, target state).
   Blocker: targeting (`targetGuid`, `focusGuid`, tab cycle) is GameHandler's
   via `ITargetingState`; make it a `TargetingHandler` (or part of Combat) so
   Spell's `currentTargetGuid`/`focusTargetGuid` have a real owner.
4. ~~QuestHandler~~ - done. Left in GameHandler: the login quest resync
   (`pendingLoginQuestResync_`), which belongs to QuestHandler's `update`.
5. ~~SocialHandler~~ - done, with `ReputationHandler` split out (standings,
   flags, watched faction, the four faction opcodes). Left in GameHandler:
   the Faction.dbc tables and the reputation list/rows built from them
   (asked through `ReputationQueries`; they move into ReputationHandler
   next), the helm/cloak flags (written by EntityController too), and the
   GM ticket opcodes (ChatHandler's commands; they belong together).
6. ~~ChatHandler~~ - done. Left in GameHandler: `chatAutoJoin` (copied into
   the handler at world entry - a mirror to remove) and the Languages.dbc
   name cache (asked through `ChatQueries::languageName`).
7. **WardenHandler** (27) - session, not a domain: it moves with the
   connection (below), not into a character scope. Coordinate with the
   reconnect work before touching it.

Not yet anyone's, to be given owners on the way: achievements
(`earnedAchievements_`, dates, name cache - an `AchievementHandler`; Spell
reaches them through `SpellQueries::achievementName/recordAchievementEarned`),
game-object interaction (`pendingGameObjectInteractGuid_`,
`lastInteractedGoGuid_`, loot-open state - shared by Spell and Inventory),
critters and companions, the home bind, corpse and resurrection, area triggers.

## Other per-character state still not reset as a whole

Found while converting, not fixed here (each needs a fail-first relog test):

- InventoryHandler: mail, bank, auction, loot-roll and trade windows are
  cleared by hand in places, not part of `Held`.
- GameHandler: achievements (earned set and dates) are never cleared on a
  character change.
- SpellHandler: known spells, cooldowns, auras and talents are reset by
  `resetAllState` field by field; fold them into `CharacterSpellState`.

## Splitting GameHandler into Connection / CharacterSession / MapScope

What GameHandler holds today falls into three lifetimes:

- **Connection** (process -> disconnect): `WorldConnection`, auth and the
  session key, Warden, opcode/update-field tables, packet parsers, the packet
  queue and dispatch table, the character list, realm extension, account data
  (`AccountDataHandler`). Implements `PacketSender`. Survives a logout to the
  character list (as `returnToCharacterSelect` already keeps the socket);
  a reconnect replaces it (see `tasks/reconnect-plan.md` on the reconnect
  branch).
- **CharacterSession** (selectCharacter -> logout): every domain handler's
  per-character struct - inventory, spells, pet, action bar, quests, social,
  reputation, achievements, PlayerState. Built in `selectCharacter`, destroyed
  at logout, so `beginCharacter`/`resetAllState` disappear: construction is the
  reset. The handlers' DBC caches (Spell.dbc, skill lines, talents, taxi) are
  not per-character and move to a process-lifetime `GameData` the sessions are
  handed.
- **MapScope** (world entry / transfer -> leaving the map): the entity
  manager and EntityController's view, transports, unit cast states and aura
  caches, area triggers, weather, world states. `leaveWorld` /
  `clearUnitCaches` become its destructor. It mirrors the client's
  `core::MapScope`, which already subscribes to the same transfer events.

Steps:

1. Finish moving state out of GameHandler into handlers (the order above),
   until GameHandler holds only the three lifetimes' wiring. Each step keeps
   GameHandler's facade for callers.
2. Split each handler's state into its per-character struct (done for
   Inventory, Pet, Spell's skills/mods/zones/totems) and its per-map part.
3. Introduce `game::CharacterSession` owning the handlers' per-character
   structs, created by `selectCharacter`; `GameHandler` holds it in a
   `unique_ptr` and the facades forward through it. Tests: the relog tests
   already in `test_*_domain.cpp` become this class's contract.
4. Introduce `game::MapScope`, created on `SMSG_LOGIN_VERIFY_WORLD` /
   `SMSG_NEW_WORLD`, destroyed on transfer and logout.
5. What remains of GameHandler is the Connection. Rename it, and move the
   facades' callers (`ui/`, `addons/`, `core/`) to the handlers' accessors
   (`gh.spells()`, `gh.inventory()`, ...), one directory at a time.

## Merging with the reconnect work

`claude/reconnect-keep-map-59ea34` changes the in-world checks of
`SpellHandler::castSpell`, `InventoryHandler::lootTarget/lootItem/
dispatchUseItem/initiateTrade` to `owner_.worldCommandAllowed("...")`. Those
handlers no longer have `owner_` on this branch: on merge, add
`worldCommandAllowed(const char*)` to `PacketSender` (GameHandler implements
it) and call `sender_.worldCommandAllowed(...)` there. Its `GameHarness`
additions (`createObjectBody`, fake connection options) sit beside this
branch's (`createItemBody`, `valuesBody`, `playerInView`, `relogAs`).
