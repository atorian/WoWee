#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::reputation {

/// The standings arrived; every unit's hostility is judged again by them.
struct StandingsInitialized {};
/// A standing moved (UPDATE_FACTION, the chat line and the toast). The name
/// is Faction.dbc's, or "faction #<id>" when the file has none.
struct StandingChanged {
    uint32_t factionId = 0;
    std::string factionName;
    int32_t delta = 0;
    int32_t standing = 0;
};
/// The server set or cleared the at-war flag; hostility is judged again.
struct AtWarChanged { uint32_t reputationIndex = 0; bool atWar = false; };

using ReputationChange = std::variant<StandingsInitialized, StandingChanged, AtWarChanged>;

} // namespace wowee::reputation
