#pragma once

#include "game/refusal.hpp"
#include "pipeline/weather_kind.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::game {
/// handler_types.hpp: DIRECTED, OMNI or AREA.
enum class SpellCastType : uint8_t;
/// server_protocol.hpp: a patch archive a realm extension names.
struct EzWowPatch;
}

/// What the game tells the rest of the client has happened, delivered through
/// core::EventBus. Plain data only: an event names what changed and carries the
/// values a subscriber needs, so a subscriber includes this header rather than
/// the class that raised it. See AGENTS.md, "Events".
///
/// One struct per fact, named for what happened (past tense or a state that
/// changed), never for who should react to it.
namespace wowee::game::events {

/// The logout can no longer be cancelled: its countdown ran out, or the server
/// logged the character out at once. The server now takes the world away -
/// everything in view goes out of range, in packets the client works through
/// over several frames - and then completes the logout. Nothing of that is the
/// player's to watch.
struct LogoutCommitted {
    bool exiting = false;
};

/// The server completed /logout or /quit. The client leaves after the frame.
struct LogoutCompleted {
    bool exiting = false;
};

/// Exit was requested without waiting for a successful server logout.
struct ExitRequested {};

/// TRANSFER_PENDING or NEW_WORLD announced the destination map.
struct WorldTransferStarted {
    uint32_t mapId = 0;
};

/// The server placed the player in a world (login, transfer or teleport).
/// Positions are server coordinates, as carried by the packet.
struct WorldEntered {
    uint32_t mapId = 0;
    float x = 0, y = 0, z = 0;
    bool isInitialEntry = false;
};

/// The world connection closed. World state remains valid until client cleanup.
struct Disconnected {
    uint64_t characterGuid = 0;
};

/// The answer to a character creation: SMSG_CHAR_CREATE, or the client's own
/// refusal when it cannot ask (not connected, the list not in yet).
struct CharacterCreateAnswered {
    bool success = false;
    std::string message;
};

/// The answer to a character deletion: SMSG_CHAR_DELETE, no answer in time, or
/// the client's own refusal when it is not connected.
struct CharacterDeleteAnswered {
    bool success = false;
    std::string message;
};

/// SMSG_CHARACTER_LOGIN_FAILED, and why, in the player's words.
struct CharacterLoginFailed {
    std::string reason;
};

/// A realm extension announced the patch archives this client should hold.
struct ServerPatchManifestReceived {
    std::vector<EzWowPatch> patches;
};

/// The player released to a graveyard (true) or came back to life (false).
struct GhostStateChanged {
    bool isGhost = false;
};

/// Another player in view reached a new level.
struct OtherPlayerLevelUp {
    uint64_t guid = 0;
    uint32_t newLevel = 0;
};

/// A creature or a player in view died: its health or its dead flag said so,
/// it came into view dead, or its corpse appeared.
struct UnitDied {
    uint64_t guid = 0;
};

/// A creature or a player in view came back to life.
struct UnitRespawned {
    uint64_t guid = 0;
};

// ---- What is in view ----

/// A transport's model is in the world: the spawner made its instance.
/// Canonical position.
struct TransportSpawned {
    uint64_t guid = 0;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    float x = 0, y = 0, z = 0;
    float orientation = 0;
};

/// The server moved a transport. Canonical position.
struct TransportMoved {
    uint64_t guid = 0;
    float x = 0, y = 0, z = 0;
    float orientation = 0;
};

/// A creature came into view, or its display changed in place. Canonical
/// position; scale is OBJECT_FIELD_SCALE_X.
struct CreatureSpawned {
    uint64_t guid = 0;
    uint32_t displayId = 0;
    float x = 0, y = 0, z = 0;
    float orientation = 0;
    float scale = 1.0f;
};

/// A creature left view.
struct CreatureDespawned {
    uint64_t guid = 0;
};

/// A unit is moving to a canonical position over durationMs (0 = at once).
struct CreatureMoved {
    uint64_t guid = 0;
    float x = 0, y = 0, z = 0;
    uint32_t durationMs = 0;
};

/// Another player - or a player's corpse - came into view, with the
/// appearance the renderer builds the body and hair textures from.
struct PlayerSpawned {
    uint64_t guid = 0;
    uint32_t displayId = 0;
    uint8_t raceId = 0;
    uint8_t genderId = 0;
    uint32_t appearanceBytes = 0;
    uint8_t facialFeatures = 0;
    float x = 0, y = 0, z = 0;
    float orientation = 0;
};

/// A player or a corpse left view.
struct PlayerDespawned {
    uint64_t guid = 0;
};

/// A player in view other than the local character mounted (the mount's
/// display id) or dismounted (0).
struct OtherPlayerMounted {
    uint64_t guid = 0;
    uint32_t mountDisplayId = 0;
};

/// A game object came into view. Canonical position; scale is
/// OBJECT_FIELD_SCALE_X.
struct GameObjectSpawned {
    uint64_t guid = 0;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    float x = 0, y = 0, z = 0;
    float orientation = 0;
    float scale = 1.0f;
};

/// A game object moved to a canonical position.
struct GameObjectMoved {
    uint64_t guid = 0;
    float x = 0, y = 0, z = 0;
    float orientation = 0;
};

/// A game object left view.
struct GameObjectDespawned {
    uint64_t guid = 0;
};

/// A game object plays a one-shot animation: SMSG_GAMEOBJECT_CUSTOM_ANIM, or a
/// fishing bobber's bite.
struct GameObjectCustomAnimPlayed {
    uint64_t guid = 0;
    uint32_t animId = 0;
};

/// A game object's state byte (GAMEOBJECT_BYTES_1) changed: 0 ready (closed),
/// 1 open, 2 destroyed.
struct GameObjectStateChanged {
    uint64_t guid = 0;
    uint8_t goState = 0;
};

/// A unit other than the player said what its movement flags are - Walk(4)
/// against Run(5), swimming, flying.
///
/// `mask` says which flags this update actually speaks about, because most
/// of the opcodes that reach here speak about one. A full movement block
/// is authoritative and passes ~0u; SMSG_SPLINE_MOVE_START_SWIM knows only
/// about swimming and passes SWIMMING, so it cannot take the flying
/// animation off a creature that is doing both.
struct UnitMoveFlagsChanged {
    uint64_t guid = 0;
    uint32_t moveFlags = 0;
    uint32_t mask = 0;
};

/// How a unit took a melee hit, for the animation it plays.
enum class HitReaction : uint8_t { WOUND, CRIT_WOUND, DODGE, PARRY, BLOCK, SHIELD_BLOCK };

/// A unit took a melee hit that did not miss.
struct UnitHitReacted {
    uint64_t victimGuid = 0;
    HitReaction reaction = HitReaction::WOUND;
};

/// An NPC turned on the player (SMSG_AI_REACTION, or its first swing at
/// them), for its aggro line. Canonical position.
struct NpcAggroed {
    uint64_t guid = 0;
    float x = 0, y = 0, z = 0;
};

// ---- The player ----

/// SMSG_STANDSTATE_UPDATE confirmed a stand state: 0 stand, 1-6 sit
/// variants, 7 dead, 8 kneel.
struct StandStateChanged {
    uint8_t standState = 0;
};

/// UNIT_FLAG_STUNNED came on or went off the player. The logout countdown's
/// stun is not one.
struct StunStateChanged {
    bool stunned = false;
};

/// The player started or stopped sneaking.
struct StealthStateChanged {
    bool stealthed = false;
};

/// The player's health changed.
struct PlayerHealthChanged {
    uint32_t health = 0;
    uint32_t maxHealth = 0;
};

/// The player got into a vehicle (its id) or out of one.
struct VehicleStateChanged {
    bool entered = false;
    uint32_t vehicleId = 0;
};

/// The player mounted (the mount's display id) or dismounted (0) - a flight
/// mount included.
struct PlayerMountChanged {
    uint32_t mountDisplayId = 0;
};

/// PLAYER_BYTES or the facial features changed (barber shop and the like).
struct PlayerAppearanceChanged {};

/// The player's model wants rebuilding with its new hair or facial hair,
/// alongside the appearance change, without a restart.
struct PlayerModelRebuildRequested {};

/// SMSG_MOVE_KNOCK_BACK for the player. vcos, vsin: the direction in server
/// (wire) space - the server to canonical to render swaps cancel, so the
/// consumer uses them in render space as they are, see
/// CameraController::applyKnockBack. hspeed and vspeed as the packet has them:
/// vspeed is negative when the server means an upward launch, so negate it
/// before using it as the initial vertical velocity.
struct PlayerKnockedBack {
    float vcos = 0, vsin = 0;
    float hspeed = 0, vspeed = 0;
};

/// SMSG_CAMERA_SHAKE: magnitude in world units, frequency in Hz, duration in
/// seconds.
struct CameraShakeRequested {
    float magnitude = 0;
    float frequency = 0;
    float duration = 0;
};

/// The player started following: renderPos is the render-space position to
/// follow, which the game keeps current. Null when following stopped.
struct AutoFollowChanged {
    const glm::vec3* renderPos = nullptr;
};

/// /unstuck: put the player back on the floor.
struct UnstuckRequested {};

/// /unstuckgy: take the player to a safe place.
struct UnstuckToGraveyardRequested {};

/// /unstuckhearth: take the player to the hearthstone's bind point.
struct UnstuckToHearthRequested {};

/// The hearthstone was bound (SMSG_BINDPOINTUPDATE). Canonical position.
struct BindPointChanged {
    uint32_t mapId = 0;
    float x = 0, y = 0, z = 0;
};

/// A flight's path, canonical, whose terrain wants loading ahead of it.
struct TaxiPathPrecacheRequested {
    std::vector<glm::vec3> path;
};

/// A flight is about to start: after the mounting delay, before it moves.
struct TaxiFlightStarted {};

/// The flight mount's rotation: yaw (render space), pitch and roll, in
/// radians.
struct TaxiOrientationChanged {
    float yaw = 0, pitch = 0, roll = 0;
};

/// The game moved the player somewhere the renderer has not followed: a
/// landing snapped to its node, or a server position taken over the client's.
/// Canonical position.
///
/// Application's per-frame render-position sync only pulls from the game while
/// on a flight (its "Sync character render position" block). Once
/// finishClientTaxiFlight() clears the flight, that sync stops, so a
/// correction applied only to movementInfo and the entity never reaches the
/// renderer, which stays at its last (wrong) position: the landing clamp read
/// renderer->getCharacterPosition() and kept computing from before the
/// correction. This carries the corrected position to the renderer at the
/// same moment.
struct PlayerPositionCorrected {
    float x = 0, y = 0, z = 0;
};

/// How drunk the player is, from 0 (sober, and on every character change) to
/// 1 (completely smashed).
struct PlayerIntoxicationChanged {
    float amount = 0.0f;
};

/// The game turned the player to face this way - at a target, to cast - in
/// canonical radians. Delivered before the facing is sent to the server, which
/// checks the cast's arc against what the client then reports.
struct PlayerFacingSet {
    float canonicalYaw = 0.0f;
};

/// The player found an area, and the experience it gave.
struct AreaDiscovered {
    std::string areaName;  ///< Empty when the area has no name in the client's tables.
    uint32_t xpGained = 0;
};

/// The player was credited honor for a kill (SMSG_PVP_CREDIT).
struct HonorGained {
    uint32_t honor = 0;
    uint64_t victimGuid = 0;
    uint32_t victimRank = 0;
};

// ---- Spells ----

/// A unit began a timed cast (SMSG_SPELL_START): the player's own, or any
/// other unit's in view.
struct SpellCastStarted {
    uint64_t casterGuid = 0;
    uint32_t spellId = 0;
    uint32_t castTimeMs = 0;
    bool byPlayer = false;
    /// A tradeskill: crafting has no cast effects.
    bool isProfession = false;
    /// Auto Shot, Shoot, Throw - drawn as a shot, not as a cast.
    bool isRangedAutoAttack = false;
};

/// A spell went off (SMSG_SPELL_GO).
struct SpellCastSucceeded {
    uint64_t casterGuid = 0;
    uint32_t spellId = 0;
    bool byPlayer = false;
    bool isProfession = false;
    bool isRangedAutoAttack = false;
    /// The player's own melee-range instant ability (a swing, not a cast).
    bool isMeleeAbility = false;
    /// Who it hit, and for a ranged auto attack the unit the shot flies at.
    std::vector<uint64_t> hitTargets;
    uint64_t projectileTarget = 0;
};

/// A unit began or ended a cast or a channel, for its animation. The cast
/// type says how it is aimed: at a unit, at nothing, or at the ground.
struct CastAnimationChanged {
    uint64_t casterGuid = 0;
    bool started = false;
    bool channel = false;
    SpellCastType castType{};
};

/// The player swung in melee: an auto attack (spellId 0) or a melee ability.
struct PlayerMeleeSwing {
    uint32_t spellId = 0;
};

/// Whether the player carries Sprint or a sprint-like aura, said with each
/// update of the player's auras.
struct SprintAuraChanged {
    bool active = false;
};

/// The player began casting the hearthstone; the bind point's terrain can be
/// loaded while the cast runs. Canonical coordinates.
struct HearthstoneCastStarted {
    uint32_t mapId = 0;
    float x = 0, y = 0, z = 0;
};

/// The player charged a target: where to, canonical coordinates.
struct ChargeStarted {
    uint64_t targetGuid = 0;
    float x = 0, y = 0, z = 0;
};

/// The character's spells arrived (SMSG_INITIAL_SPELLS), the first thing the
/// realm sends of a character entering the world.
struct CharacterSpellsArrived {};

/// The player's cast ended without going off.
struct PlayerCastStopped {
    /// Refused or interrupted by the server, rather than cancelled by the
    /// player: the cast's own sound is cut short too.
    bool failed = false;
};

/// The player's own cast is over, however it ended - for what waits on it:
/// opening a game object waits on the cast that opens it.
struct PlayerCastEnded {
    enum class Outcome {
        WentOff,    ///< SMSG_SPELL_GO
        Failed,     ///< refused or interrupted by the server
        Cancelled,  ///< the player stopped it
        Reset,      ///< the cast state was dropped without an answer
    };
    uint32_t spellId = 0;
    Outcome outcome = Outcome::Failed;
    /// For WentOff: the cast had a cast bar, rather than going off at once.
    bool timed = false;
};

/// The server asked for a spell visual on a unit (SMSG_PLAY_SPELL_VISUAL,
/// SMSG_PLAY_SPELL_IMPACT).
struct SpellVisualRequested {
    uint64_t unitGuid = 0;
    uint32_t visualId = 0;
    /// The visual's impact kit rather than its cast kit.
    bool impactKit = false;
};

/// Spells the player can now cast - the whole book on login, or one learned.
struct SpellsLearned {
    std::vector<uint32_t> spellIds;
};

/// The player's own cast was refused (SMSG_CAST_FAILED). `routine` marks the
/// refusals that repeat with every keypress ("Not ready yet").
struct PlayerCastFailed {
    uint32_t spellId = 0;
    bool routine = false;
};

/// The player reached a new level (SMSG_LEVELUP_INFO).
struct PlayerLevelUp {
    uint32_t oldLevel = 0;
    uint32_t newLevel = 0;
};

/// A line for the player's error frame: a refusal ("Not enough mana"), or a
/// script that failed (`scriptError`), which is not a Lua event.
struct UiErrorRaised {
    std::string message;
    bool scriptError = false;
};

/// The game refused something the player asked for, for a reason the
/// character says aloud.
struct PlayerRefused {
    Refusal reason = Refusal::CantUseItem;
};

/// An item arrived in the player's bags - looted, a reward, pushed by the
/// server - with what it is, once its name is known.
struct ItemReceived {
    uint32_t itemId = 0;
    uint32_t count = 0;
    uint32_t quality = 0;
    std::string name;
};

/// The player sold an item to a vendor.
struct ItemSold {
    uint64_t itemGuid = 0;
};

/// The player bought an item from a vendor, new or back from the buyback list.
struct ItemBought {
    uint32_t itemId = 0;
    uint32_t count = 0;
};

/// A quest objective moved: a kill, an item, counted against what is needed.
struct QuestObjectiveProgressed {
    std::string questTitle;
    std::string objective;
    uint32_t count = 0;
    uint32_t required = 0;
};

/// The quests the watch frame shows changed; the character config keeps them.
struct QuestTrackingChanged {};

/// A gossip window opened on an NPC in view, which greets the player.
/// Canonical coordinates.
struct GossipOpenedOnNpc {
    uint64_t npcGuid = 0;
    float x = 0, y = 0, z = 0;
};

/// The gossip window on a creature in view closed: it bids the player
/// farewell. Canonical coordinates.
struct GossipClosedOnNpc {
    uint64_t npcGuid = 0;
    float x = 0, y = 0, z = 0;
};

/// The server opened an NPC's gossip page or quest list
/// (SMSG_GOSSIP_MESSAGE, SMSG_QUESTGIVER_QUEST_LIST), in view or not. It
/// takes the place of any vendor window.
struct GossipWindowOpened {
    uint64_t npcGuid = 0;
};

/// The server opened one of an NPC's service windows - its goods, a
/// trainer's list, the auction house, the flight map. It takes the gossip
/// page's place.
struct NpcWindowOpened {
    uint64_t npcGuid = 0;
};

/// The player chose a gossip option that opens the NPC's goods; `repairs`
/// when it is the armorer's, whose vendor window can repair.
struct VendorOptionChosen {
    uint64_t npcGuid = 0;
    bool repairs = false;
};

/// The player chose the stable master's gossip option: the stable that
/// opens next is this NPC's.
struct StableOptionChosen {
    uint64_t npcGuid = 0;
};

/// The server asks the dungeon finder be opened (SMSG_OPEN_LFG_DUNGEON_FINDER).
struct DungeonFinderOpened {};

/// The player accepted a quest into the log.
struct QuestAccepted {
    uint32_t questId = 0;
};

/// The player turned a quest in, and it left the log.
struct QuestCompleted {
    uint32_t questId = 0;
    std::string title;
};

/// The player learned a spell from a trainer.
struct TrainerSpellLearned {
    uint32_t spellId = 0;
};

/// Another player pinged the minimap, at a point in WoW coordinates.
struct MinimapPinged {
    uint64_t senderGuid = 0;
    float wowX = 0.0f;
    float wowY = 0.0f;
};

/// A fish took the player's lure.
struct FishingBite {
    uint64_t bobberGuid = 0;
};

/// The player became a unit's top threat, while threat warnings with sound
/// are wanted.
struct ThreatWarningRaised {
    uint64_t unitGuid = 0;
};

/// A melee swing went in: landed, missed, or was dodged or parried.
struct MeleeSwingResolved {
    uint64_t attackerGuid = 0;
    uint64_t victimGuid = 0;
    bool attackerIsPlayer = false;
    bool victimIsPlayer = false;
    bool missed = false;
    /// Dodged or parried: the swing went through the air, and nothing was hit.
    bool avoided = false;
    bool critical = false;
};

/// One slot of the account data now holds this text - read from the local
/// cache or sent by the server. `type` is a game::AccountDataType; the key
/// bindings slots are what the interface answers by reloading its bindings.
struct AccountDataLoaded {
    uint8_t type = 0;
    std::string text;
};

/// The character no longer knows these spells (SMSG_REMOVED_SPELL,
/// SMSG_SEND_UNLEARN_SPELLS). The action bar empties the buttons that held
/// them.
struct SpellsUnlearned {
    std::vector<uint32_t> spellIds;
};

/// A spell was replaced by its next rank (SMSG_SUPERCEDED_SPELL). The action
/// bar's buttons that held the old rank hold the new one.
struct SpellSuperseded {
    uint32_t oldSpellId = 0;
    uint32_t newSpellId = 0;
};

/// A player's name is known now (SMSG_NAME_QUERY_RESPONSE): what waited for
/// it - a chat line, a mail's sender - can be finished.
struct PlayerNameKnown {
    uint64_t guid = 0;
    std::string name;
};

/// A unit said or yelled a line that floats over its head as a bubble - a
/// say, a yell, a monster's line, and party chat when the player wants that
/// too.
struct UnitSaidAloud {
    uint64_t guid = 0;
    std::string text;
    bool yell = false;
};

/// A unit plays an emote's animation (SMSG_EMOTE, the emote state field, a
/// drowning hit...). `looping` for a STATE_ emote that holds until another
/// replaces it; animation 0 cancels a one-shot.
struct UnitEmotePlayed {
    uint64_t guid = 0;
    uint32_t animationId = 0;
    bool looping = false;
};

/// A macro was deleted from the account data. The action bar empties the
/// buttons that held it.
struct MacroDeleted {
    uint32_t macroId = 0;
};

// ---- Items ----

/// What the player carries in the backpack and the four worn bags, summed per
/// item entry - the set the server checks at a quest turn-in. Said whenever
/// the bags are rebuilt; the quest log's "collect N" objectives count from it,
/// since 3.3.5a never sends those counts itself.
struct CarriedItemsCounted {
    std::unordered_map<uint32_t, uint32_t> counts;
};

/// The player opened a vendor's window. The position is the vendor's, in
/// canonical coordinates.
struct VendorOpened {
    uint64_t vendorGuid = 0;
    float x = 0, y = 0, z = 0;
};

/// What a player in view is wearing, resolved to display ids: nineteen slots
/// in the visible-item order (head, neck, shoulders ... tabard), zero where
/// nothing is worn or nothing is known.
struct PlayerEquipmentResolved {
    uint64_t guid = 0;
    std::array<uint32_t, 19> displayIds{};
    std::array<uint8_t, 19> inventoryTypes{};
    /// The SpellItemEnchantment id each slot shows - a temporary enchant over
    /// the permanent one, as InventoryHandler::shownEnchantId has it for the
    /// player's own items - or zero. The glow on a weapon comes from this.
    std::array<uint32_t, 19> enchantIds{};
};

/// The server said what its wall clock reads, in seconds since 1970 - the
/// reply to CMSG_QUERY_TIME. The realm clock keeps time from it.
struct ServerTimeReceived {
    uint32_t unixSeconds = 0;
};

/// The realm said what the weather is where the character stands
/// (SMSG_WEATHER): a new kind, or the same one harder or softer.
struct WeatherChanged {
    pipeline::WeatherKind kind = pipeline::WeatherKind::Clear;
    float intensity = 0.0f;  ///< 0-1
    /// It arrives at once rather than coming over.
    bool abrupt = false;
    /// The kind it was before.
    pipeline::WeatherKind previous = pipeline::WeatherKind::Clear;
};

// ---- Sound ----

/// SMSG_PLAY_MUSIC: a SoundEntries.dbc record, which the receiver looks up
/// and hands to the music player.
struct MusicRequested {
    uint32_t soundId = 0;
};

/// SMSG_PLAY_SOUND: a SoundEntries.dbc record, played flat.
struct SoundRequested {
    uint32_t soundId = 0;
};

/// SMSG_PLAY_OBJECT_SOUND: a SoundEntries.dbc record, played where
/// sourceGuid stands.
struct PositionalSoundRequested {
    uint32_t soundId = 0;
    uint64_t sourceGuid = 0;
};

} // namespace wowee::game::events
