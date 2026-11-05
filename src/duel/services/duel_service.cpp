#include "duel/services/duel_service.hpp"

#include "foundation/event_bus.hpp"

#include <cstdio>

namespace wowee::duel {

DuelService::DuelService(IDuelGateway& gateway, IDuelWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

void DuelService::apply(const std::vector<DuelChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void DuelService::acceptDuel() {
    if (!duel_.pending() || !gateway_.online()) return;
    duel_.answered();
    gateway_.sendAccept(duel_.arbiter());
    events_.publish(DuelAccepted{});
}

void DuelService::forfeitDuel() {
    if (!gateway_.online()) return;
    duel_.answered();
    gateway_.sendCancel(duel_.arbiter());
    events_.publish(DuelForfeited{});
}

void DuelService::proposeDuel(uint64_t targetGuid) {
    if (!gateway_.online()) return;
    if (targetGuid == 0) {
        events_.publish(NoDuelTarget{});
        return;
    }
    gateway_.sendPropose(targetGuid);
    events_.publish(DuelProposed{});
}

void DuelService::onRequested(uint64_t arbiter, uint64_t challenger) {
    std::string name = world_.name(challenger);
    if (name.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX", static_cast<unsigned long long>(challenger));
        name = tmp;
    }
    apply(duel_.challenged(arbiter, challenger, world_.playerGuid(), std::move(name)));
}

void DuelService::onWinner(bool fled, std::string winner, std::string loser) {
    events_.publish(DuelWinner{fled, std::move(winner), std::move(loser)});
}

void DuelService::onOutOfBounds() { events_.publish(DuelOutOfBounds{}); }
void DuelService::onInBounds() { events_.publish(DuelInBounds{}); }

} // namespace wowee::duel
