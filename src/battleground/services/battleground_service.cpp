#include "battleground/services/battleground_service.hpp"

#include "foundation/event_bus.hpp"

namespace wowee::battleground {

BattlegroundService::BattlegroundService(IBattlegroundGateway& gateway, IBattlegroundWorld& world,
                                         core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

void BattlegroundService::apply(const std::vector<BattlegroundChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void BattlegroundService::acceptBattlefield(uint32_t queueSlot) {
    if (!gateway_.online()) return;
    const auto slot = bgs_.invitation(queueSlot);
    if (!slot) {
        events_.publish(NoInvitation{});
        return;
    }
    gateway_.sendPort(slot->arenaType, slot->bgTypeId, true);
    bgs_.entering(*slot);
    events_.publish(EnteringBattleground{});
}

void BattlegroundService::declineBattlefield(uint32_t queueSlot) {
    if (!gateway_.online()) return;
    const auto slot = bgs_.leavable(queueSlot);
    if (!slot) {
        events_.publish(NoInvitation{});
        return;
    }
    gateway_.sendPort(slot->arenaType, slot->bgTypeId, false);
    apply(bgs_.declined(*slot));
}

void BattlegroundService::leaveBattlefield() {
    if (gateway_.online()) gateway_.sendLeave();
}

void BattlegroundService::requestBattlefieldList(uint32_t bgTypeId) {
    if (gateway_.online()) gateway_.sendListRequest(bgTypeId);
}

void BattlegroundService::reportPvpAfk(uint64_t playerGuid) {
    if (gateway_.online() && playerGuid != 0) gateway_.sendReportAfk(playerGuid);
}

void BattlegroundService::joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                                          uint32_t instanceId, bool asGroup) {
    if (!gateway_.online()) return;
    gateway_.sendJoin(battlemasterGuid, bgTypeId, instanceId, asGroup);
    bgs_.joinRequested(bgTypeId);
}

void BattlegroundService::requestPvpLog() {
    if (gateway_.online()) gateway_.sendScoreRequest();
}

void BattlegroundService::requestBattlefieldPositions() {
    if (!gateway_.online() || !world_.inBattleground()) return;
    if (bgs_.positionsRequestDue(Battlegrounds::Clock::now())) gateway_.sendPositionsRequest();
}

void BattlegroundService::onStatus(const BattlefieldStatus& status) {
    apply(bgs_.statusReceived(status, Battlegrounds::Clock::now()));
}

void BattlegroundService::onGroupQueued(uint32_t bgTypeId) {
    bgs_.joinAnswered();
    events_.publish(GroupQueued{bgTypeId});
}

void BattlegroundService::onJoinRefused(JoinRefusal reason, int32_t code, std::optional<uint64_t> member) {
    bgs_.joinAnswered();
    std::string who;
    if (member) who = world_.playerName(*member);
    if (who.empty()) who = "A party member";
    events_.publish(JoinRefused{reason, code, std::move(who)});
}

void BattlegroundService::onPortDenied() { events_.publish(PortDenied{}); }
void BattlegroundService::onRemovedFromQueue() { events_.publish(RemovedFromQueue{}); }
void BattlegroundService::onJoinedQueue() { events_.publish(JoinedQueue{}); }

void BattlegroundService::onPlayerEntered(uint64_t guid) {
    if (std::string name = world_.playerName(guid); !name.empty()) events_.publish(PlayerEntered{name});
}

void BattlegroundService::onPlayerLeft(uint64_t guid) {
    if (std::string name = world_.playerName(guid); !name.empty()) events_.publish(PlayerLeft{name});
}

void BattlegroundService::onAfkReportResult(uint8_t result) {
    events_.publish(AfkReportAnswered{result == 0});
}

} // namespace wowee::battleground
