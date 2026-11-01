#pragma once

#include "arena/domain/events.hpp"
#include "arena/domain/records.hpp"

#include <string>
#include <vector>

namespace wowee::arena {

/// The player's arena teams: their standings, and the rosters asked for.
class ArenaTeams {
public:
    std::vector<ArenaChange> named(uint32_t teamId, std::string name, uint32_t teamType);
    /// SMSG_ARENA_TEAM_STATS carries no name; the name already known stays.
    void statsReceived(ArenaTeamStats stats);
    std::vector<ArenaChange> rosterReceived(ArenaTeamRoster roster);
    /// Reorder every roster by one of the details frame's columns: "name",
    /// "class", "played", "won", "rating", "seasonplayed" or "seasonwon". The
    /// same column again reverses it; a new one starts the way it reads best,
    /// a name from A and a number from the top. Every roster, because the
    /// interface sorts what it shows without saying which team.
    void sortRosters(const std::string& key);
    /// A character is logging in: the last one's teams and rosters are not
    /// its own. The column last sorted by is the interface's and stays.
    void beginCharacter();

    [[nodiscard]] const std::vector<ArenaTeamStats>& stats() const { return stats_; }
    [[nodiscard]] const ArenaTeamRoster* roster(uint32_t teamId) const;

private:
    std::vector<ArenaTeamStats> stats_;
    std::vector<ArenaTeamRoster> rosters_;
    std::string sortKey_;
    bool sortAscending_ = true;
};

} // namespace wowee::arena
