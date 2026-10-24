#include "arena/domain/arena_teams.hpp"

#include <algorithm>
#include <utility>

namespace wowee::arena {

std::vector<ArenaChange> ArenaTeams::named(uint32_t teamId, std::string name, uint32_t teamType) {
    auto it = std::find_if(stats_.begin(), stats_.end(), [teamId](const auto& s) { return s.teamId == teamId; });
    if (it == stats_.end()) {
        ArenaTeamStats stub;
        stub.teamId = teamId;
        it = stats_.insert(stats_.end(), std::move(stub));
    }
    it->teamName = std::move(name);
    it->teamType = teamType;
    return {TeamsUpdated{}};
}

void ArenaTeams::statsReceived(ArenaTeamStats stats) {
    for (auto& s : stats_) {
        if (s.teamId != stats.teamId) continue;
        stats.teamName = std::move(s.teamName);
        stats.teamType = s.teamType;
        s = std::move(stats);
        return;
    }
    stats_.push_back(std::move(stats));
}

std::vector<ArenaChange> ArenaTeams::rosterReceived(ArenaTeamRoster roster) {
    auto it = std::find_if(rosters_.begin(), rosters_.end(),
                           [&roster](const auto& r) { return r.teamId == roster.teamId; });
    if (it != rosters_.end()) *it = std::move(roster);
    else rosters_.push_back(std::move(roster));
    return {RosterUpdated{}};
}

void ArenaTeams::sortRosters(const std::string& key) {
    if (key.empty()) return;
    // The same column twice reverses. A different one starts again in the
    // direction that column reads best: a name from A, a number from the top.
    if (key == sortKey_) {
        sortAscending_ = !sortAscending_;
    } else {
        sortKey_ = key;
        sortAscending_ = (key == "name" || key == "class");
    }
    const bool asc = sortAscending_;

    auto value = [&key](const ArenaTeamMember& m) -> uint64_t {
        if (key == "class")        return m.classId;
        if (key == "played")       return m.weekGames;
        if (key == "won")          return m.weekWins;
        if (key == "seasonplayed") return m.seasonGames;
        if (key == "seasonwon")    return m.seasonWins;
        if (key == "rating")       return m.personalRating;
        return 0;
    };

    for (auto& roster : rosters_) {
        std::stable_sort(roster.members.begin(), roster.members.end(),
            [&](const ArenaTeamMember& a, const ArenaTeamMember& b) {
                if (key == "name") return asc ? (a.name < b.name) : (b.name < a.name);
                const uint64_t va = value(a), vb = value(b);
                if (va == vb) return a.name < b.name;   // steady under ties
                return asc ? (va < vb) : (vb < va);
            });
    }
}

void ArenaTeams::beginCharacter() {
    stats_.clear();
    rosters_.clear();
}

const ArenaTeamRoster* ArenaTeams::roster(uint32_t teamId) const {
    for (const auto& r : rosters_)
        if (r.teamId == teamId) return &r;
    return nullptr;
}

} // namespace wowee::arena
