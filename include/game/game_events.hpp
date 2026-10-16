#pragma once

#include "game/refusal.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::game {
/// handler_types.hpp: DIRECTED, OMNI or AREA.
enum class SpellCastType : uint8_t;
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

/// A character is being logged in: CMSG_PLAYER_LOGIN is going out. What the
/// server keeps of a character - its party among it - comes afresh with the
/// login, so nothing a domain holds from the last session (another
/// character's, or this one's before a lost connection) stands for it.
struct CharacterLoggingIn {
    uint64_t characterGuid = 0;
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

// ---- The player ----

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

/// Money the player looted, in copper. Once per loot.
struct LootMoneyReceived {
    uint64_t lootGuid = 0;
    uint32_t copper = 0;
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

/// Unread mail arrived, where there was none.
struct MailArrived {};

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

/// The player earned an achievement.
struct AchievementEarned {
    uint32_t achievementId = 0;
    std::string name;
};

/// Someone challenged the player to a duel.
struct DuelRequested {
    uint64_t challengerGuid = 0;
    std::string challengerName;
};

/// Someone invited the player to a group.
struct GroupInviteReceived {
    std::string inviterName;
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

/// The loot window opened on a corpse or a chest, or closed.
struct LootWindowChanged {
    bool open = false;
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
};

} // namespace wowee::game::events
