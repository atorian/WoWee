#include "lfg/services/lfg_service.hpp"

#include "foundation/event_bus.hpp"

namespace wowee::lfg {

LfgService::LfgService(ILfgGateway& gateway, ILfgWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

void LfgService::apply(const std::vector<LfgChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void LfgService::lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) {
    if (!gateway_.online()) return;
    if (dungeonIds.empty()) return;   // the server drops a join with no slots
    gateway_.sendJoin(dungeonIds, roles);
}

void LfgService::lfgLeave() {
    if (!gateway_.connected()) return;
    finder_.left();
    gateway_.sendLeave();
}

void LfgService::lfgSetRoles(uint8_t roles) {
    apply(finder_.rolesOffered(roles));
    if (gateway_.online()) gateway_.sendSetRoles(roles);
}

void LfgService::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (gateway_.connected()) gateway_.sendProposalAnswer(proposalId, accept);
}

void LfgService::lfgSetBootVote(bool vote) {
    if (gateway_.connected()) gateway_.sendBootVote(vote);
}

void LfgService::lfgTeleport(bool toLfgDungeon) {
    if (gateway_.connected()) gateway_.sendTeleport(toLfgDungeon);
}

void LfgService::requestLfgPlayerLockInfo() {
    if (gateway_.online()) gateway_.sendPlayerLockInfoRequest();
}

void LfgService::requestLfgPartyLockInfo() {
    if (gateway_.online()) gateway_.sendPartyLockInfoRequest();
}

void LfgService::onBootVote(BootVote vote) {
    // The packet names the victim by guid; the name is resolved here, and
    // asked for when it is not known yet.
    std::string name = world_.playerName(vote.victimGuid);
    if (name.empty() && vote.victimGuid != 0) world_.requestPlayerName(vote.victimGuid);
    apply(finder_.bootVoteReceived(std::move(vote), std::move(name)));
}

void LfgService::onTeleportDenied(TeleportRefusal reason) {
    events_.publish(TeleportDenied{reason});
}

void LfgService::onDisabled() {
    events_.publish(FinderDisabled{});
}

void LfgService::onContinueOffered(uint32_t dungeonId, uint32_t typeId) {
    events_.publish(ContinueOffered{dungeonId, typeId});
}

void LfgService::onRoleChosen(uint64_t playerGuid, bool ready, uint32_t roles) {
    events_.publish(RoleChosen{world_.unitName(playerGuid).value_or("A player"), ready, roles});
}

} // namespace wowee::lfg
