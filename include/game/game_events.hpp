#pragma once

#include "game/refusal.hpp"

#include <cstdint>
#include <string>
#include <vector>

/// What the game tells the rest of the client has happened, delivered through
/// core::EventBus. Plain data only: an event names what changed and carries the
/// values a subscriber needs, so a subscriber includes this header rather than
/// the class that raised it. See AGENTS.md, "Events".
///
/// One struct per fact, named for what happened (past tense or a state that
/// changed), never for who should react to it.
namespace wowee::game::events {

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

} // namespace wowee::game::events
