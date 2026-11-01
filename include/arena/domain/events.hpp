#pragma once

#include "arena/domain/records.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::arena {

// What happened to the player's arena teams. ArenaTeams returns the first
// two; the service publishes them and the server's answers, and the chat log
// and the addon layer subscribe.

/// A team's name arrived (ARENA_TEAM_UPDATE): the PvP panel redraws its arena
/// tabs on it.
struct TeamsUpdated {};
/// A roster arrived (ARENA_TEAM_ROSTER_UPDATE).
struct RosterUpdated {};

using ArenaChange = std::variant<TeamsUpdated, RosterUpdated>;

// The server's answers and notices. A create names the team, an invite the
// player, and both are sent for the ones that concern both. `code` is the
// server's own number, for an outcome this client has no words for.
struct ArenaCommandSucceeded {
    ArenaCommand command = ArenaCommand::Other;
    std::string teamName;
    std::string playerName;
};
struct ArenaCommandRefused {
    ArenaCommand command = ArenaCommand::Other;
    std::string teamName;
    std::string playerName;
    ArenaCommandError error = ArenaCommandError::Other;
};
struct InvitedToArenaTeam { std::string inviter; std::string teamName; };
/// `first` and `second` are the names the event carries, which may be absent.
struct ArenaTeamChanged {
    ArenaTeamChange change = ArenaTeamChange::Other;
    std::string first;
    std::string second;
    uint32_t code = 0;
};
struct ArenaMatchRefused { ArenaMatchError error = ArenaMatchError::Other; uint32_t code = 0; };

} // namespace wowee::arena
