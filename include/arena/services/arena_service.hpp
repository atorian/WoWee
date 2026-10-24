#pragma once

#include "arena/domain/arena_teams.hpp"
#include "arena/services/arena.hpp"
#include "arena/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::arena {

class ArenaService final : public IArenaTeams, public IArenaTeamsFeed {
public:
    ArenaService(IArenaGateway& gateway, core::EventBus& events);

    // IArenaTeams: reading
    [[nodiscard]] const std::vector<ArenaTeamStats>& getArenaTeamStats() const override { return teams_.stats(); }
    [[nodiscard]] const ArenaTeamRoster* getArenaTeamRoster(uint32_t teamId) const override { return teams_.roster(teamId); }
    void sortArenaTeamRosters(const std::string& key) override { teams_.sortRosters(key); }

    // IArenaTeams: requests
    void acceptArenaTeamInvite() override;
    void declineArenaTeamInvite() override;
    void arenaTeamInvite(uint32_t teamId, const std::string& name) override;
    void arenaTeamLeave(uint32_t teamId) override;
    void arenaTeamRemove(uint32_t teamId, const std::string& name) override;
    void arenaTeamSetLeader(uint32_t teamId, const std::string& name) override;
    void disbandArenaTeam(uint32_t teamId) override;
    void requestArenaTeamRoster(uint32_t teamId) override;

    // IArenaTeamsFeed
    void onTeamNamed(uint32_t teamId, std::string name, uint32_t teamType) override {
        apply(teams_.named(teamId, std::move(name), teamType));
    }
    void onStats(ArenaTeamStats stats) override { teams_.statsReceived(std::move(stats)); }
    void onRoster(ArenaTeamRoster roster) override { apply(teams_.rosterReceived(std::move(roster))); }
    void onCommandSucceeded(ArenaCommand command, std::string teamName, std::string playerName) override;
    void onCommandRefused(ArenaCommand command, std::string teamName, std::string playerName,
                          ArenaCommandError error) override;
    void onInvited(std::string inviter, std::string teamName) override;
    void onTeamChanged(ArenaTeamChange change, std::string first, std::string second, uint32_t code) override;
    void onMatchRefused(ArenaMatchError error, uint32_t code) override;
    void beginCharacter() override { teams_.beginCharacter(); }

private:
    void apply(const std::vector<ArenaChange>& changes);

    IArenaGateway& gateway_;
    core::EventBus& events_;
    ArenaTeams teams_;
};

} // namespace wowee::arena
