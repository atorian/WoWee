#pragma once

#include <cstdint>

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

} // namespace wowee::game::events
