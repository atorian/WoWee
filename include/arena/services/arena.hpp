#pragma once

#include "arena/domain/records.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::arena {

/// Arena teams as the interface and the addon layer use them.
class IArenaTeams {
public:
    virtual ~IArenaTeams() = default;

    // ---- Reading ----
    [[nodiscard]] virtual const std::vector<ArenaTeamStats>& getArenaTeamStats() const = 0;
    /// The roster for a team, or null before it arrived.
    [[nodiscard]] virtual const ArenaTeamRoster* getArenaTeamRoster(uint32_t teamId) const = 0;
    virtual void sortArenaTeamRosters(const std::string& key) = 0;

    // ---- Requests ----
    // Accept and decline carry no payload: the server answers from the invite
    // it is already holding against this character.
    virtual void acceptArenaTeamInvite() = 0;
    virtual void declineArenaTeamInvite() = 0;
    virtual void arenaTeamInvite(uint32_t teamId, const std::string& name) = 0;
    virtual void arenaTeamLeave(uint32_t teamId) = 0;
    virtual void arenaTeamRemove(uint32_t teamId, const std::string& name) = 0;
    virtual void arenaTeamSetLeader(uint32_t teamId, const std::string& name) = 0;
    virtual void disbandArenaTeam(uint32_t teamId) = 0;
    virtual void requestArenaTeamRoster(uint32_t teamId) = 0;
};

/// What the protocol layer tells the arena teams.
class IArenaTeamsFeed {
public:
    virtual ~IArenaTeamsFeed() = default;

    /// SMSG_ARENA_TEAM_QUERY_RESPONSE.
    virtual void onTeamNamed(uint32_t teamId, std::string name, uint32_t teamType) = 0;
    /// SMSG_ARENA_TEAM_STATS.
    virtual void onStats(ArenaTeamStats stats) = 0;
    /// SMSG_ARENA_TEAM_ROSTER.
    virtual void onRoster(ArenaTeamRoster roster) = 0;
    /// SMSG_ARENA_TEAM_COMMAND_RESULT.
    virtual void onCommandSucceeded(ArenaCommand command, std::string teamName, std::string playerName) = 0;
    virtual void onCommandRefused(ArenaCommand command, std::string teamName, std::string playerName,
                                  ArenaCommandError error) = 0;
    /// SMSG_ARENA_TEAM_INVITE.
    virtual void onInvited(std::string inviter, std::string teamName) = 0;
    /// SMSG_ARENA_TEAM_EVENT with up to two names; `code` is the server's.
    virtual void onTeamChanged(ArenaTeamChange change, std::string first, std::string second, uint32_t code) = 0;
    /// SMSG_ARENA_ERROR.
    virtual void onMatchRefused(ArenaMatchError error, uint32_t code) = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::arena
