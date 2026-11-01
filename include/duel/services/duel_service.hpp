#pragma once

#include "duel/domain/duel.hpp"
#include "duel/services/duels.hpp"
#include "duel/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::duel {

struct DuelAccepted {};
struct DuelForfeited {};
struct DuelProposed {};
/// proposeDuel without anyone to challenge.
struct NoDuelTarget {};
struct DuelWinner { bool fled = false; std::string winner; std::string loser; };
/// Out of the duel area: the interface runs the ten-second countdown to a
/// forfeit off this and DuelInBounds.
struct DuelOutOfBounds {};
struct DuelInBounds {};

class DuelService final : public IDuels, public IDuelsFeed {
public:
    DuelService(IDuelGateway& gateway, IDuelWorld& world, core::EventBus& events);

    // IDuels
    [[nodiscard]] float getDuelCountdownRemaining() const override {
        return duel_.countdownRemaining(Duel::Clock::now());
    }
    void acceptDuel() override;
    void forfeitDuel() override;
    void proposeDuel(uint64_t targetGuid) override;

    // IDuelsFeed
    void onRequested(uint64_t arbiter, uint64_t challenger) override;
    void onCountdown(uint32_t ms) override { duel_.countdownStarted(ms, Duel::Clock::now()); }
    void onComplete(bool started) override { apply(duel_.completed(started)); }
    void onWinner(bool fled, std::string winner, std::string loser) override;
    void onOutOfBounds() override;
    void onInBounds() override;
    void beginCharacter() override { duel_.beginCharacter(); }

private:
    void apply(const std::vector<DuelChange>& changes);

    IDuelGateway& gateway_;
    IDuelWorld& world_;
    core::EventBus& events_;
    Duel duel_;
};

} // namespace wowee::duel
