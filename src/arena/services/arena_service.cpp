#include "arena/services/arena_service.hpp"

#include "core/event_bus.hpp"

namespace wowee::arena {

ArenaService::ArenaService(IArenaGateway& gateway, core::EventBus& events)
    : gateway_(gateway), events_(events) {}

void ArenaService::apply(const std::vector<ArenaChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void ArenaService::acceptArenaTeamInvite() {
    if (gateway_.online()) gateway_.sendAccept();
}

void ArenaService::declineArenaTeamInvite() {
    if (gateway_.online()) gateway_.sendDecline();
}

void ArenaService::arenaTeamInvite(uint32_t teamId, const std::string& name) {
    if (teamId != 0 && !name.empty() && gateway_.online()) gateway_.sendInvite(teamId, name);
}

void ArenaService::arenaTeamLeave(uint32_t teamId) {
    if (teamId != 0 && gateway_.online()) gateway_.sendLeave(teamId);
}

void ArenaService::arenaTeamRemove(uint32_t teamId, const std::string& name) {
    if (teamId != 0 && !name.empty() && gateway_.online()) gateway_.sendRemove(teamId, name);
}

void ArenaService::arenaTeamSetLeader(uint32_t teamId, const std::string& name) {
    if (teamId != 0 && !name.empty() && gateway_.online()) gateway_.sendLeader(teamId, name);
}

void ArenaService::disbandArenaTeam(uint32_t teamId) {
    if (teamId != 0 && gateway_.online()) gateway_.sendDisband(teamId);
}

void ArenaService::requestArenaTeamRoster(uint32_t teamId) {
    if (gateway_.connected()) gateway_.sendRosterRequest(teamId);
}

void ArenaService::onCommandSucceeded(ArenaCommand command, std::string teamName, std::string playerName) {
    events_.publish(ArenaCommandSucceeded{command, std::move(teamName), std::move(playerName)});
}

void ArenaService::onCommandRefused(ArenaCommand command, std::string teamName, std::string playerName,
                                    ArenaCommandError error) {
    events_.publish(ArenaCommandRefused{command, std::move(teamName), std::move(playerName), error});
}

void ArenaService::onInvited(std::string inviter, std::string teamName) {
    events_.publish(InvitedToArenaTeam{std::move(inviter), std::move(teamName)});
}

void ArenaService::onTeamChanged(ArenaTeamChange change, std::string first, std::string second, uint32_t code) {
    events_.publish(ArenaTeamChanged{change, std::move(first), std::move(second), code});
}

void ArenaService::onMatchRefused(ArenaMatchError error, uint32_t code) {
    events_.publish(ArenaMatchRefused{error, code});
}

} // namespace wowee::arena
