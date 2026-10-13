#pragma once

#include <cstdint>
#include <vector>

/// What the game tells the rest of the client has happened, delivered through
/// core::EventBus. Plain data only: an event names what changed and carries the
/// values a subscriber needs, so a subscriber includes this header rather than
/// the class that raised it. See AGENTS.md, "Events".
///
/// One struct per fact, named for what happened (past tense or a state that
/// changed), never for who should react to it.
namespace wowee::game::events {

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

} // namespace wowee::game::events
