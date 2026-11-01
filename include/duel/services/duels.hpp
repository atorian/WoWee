#pragma once

#include <cstdint>
#include <string>

namespace wowee::duel {

/// Duels as the interface and the addon layer use them.
class IDuels {
public:
    virtual ~IDuels() = default;

    /// Seconds left before the duel starts, or zero.
    [[nodiscard]] virtual float getDuelCountdownRemaining() const = 0;
    virtual void acceptDuel() = 0;
    virtual void forfeitDuel() = 0;
    virtual void proposeDuel(uint64_t targetGuid) = 0;
};

/// What the protocol layer tells duels.
class IDuelsFeed {
public:
    virtual ~IDuelsFeed() = default;

    /// SMSG_DUEL_REQUESTED: the flag, then the challenger.
    virtual void onRequested(uint64_t arbiter, uint64_t challenger) = 0;
    virtual void onCountdown(uint32_t ms) = 0;
    virtual void onComplete(bool started) = 0;
    /// `fled` when the loser ran rather than lost.
    virtual void onWinner(bool fled, std::string winner, std::string loser) = 0;
    virtual void onOutOfBounds() = 0;
    virtual void onInBounds() = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::duel
