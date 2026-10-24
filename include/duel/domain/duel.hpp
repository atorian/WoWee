#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace wowee::duel {

/// Someone challenged this player (DUEL_REQUESTED, and the challenge sound).
struct DuelRequested { std::string challengerName; };
/// The duel ended (DUEL_FINISHED); `started` is false for one called off
/// before it began.
struct DuelFinished { bool started = false; };

using DuelChange = std::variant<DuelRequested, DuelFinished>;

/// A duel this player is asked to, or in: the flag both answers name, who
/// challenged, and the countdown before it starts.
class Duel {
public:
    using Clock = std::chrono::steady_clock;

    /// The server sends a challenge to both players. The one who issued it
    /// keeps the flag, to be able to call it off, and is not asked to accept.
    std::vector<DuelChange> challenged(uint64_t arbiter, uint64_t challenger, uint64_t self,
                                       std::string challengerName);
    /// The answer went out; nothing is waiting for one any more.
    void answered() { pending_ = false; }
    /// A countdown out of range is taken as the usual three seconds.
    void countdownStarted(uint32_t ms, Clock::time_point now);
    std::vector<DuelChange> completed(bool started);
    /// A character is logging in: a duel put to the last one, its flag and
    /// its countdown are not this one's.
    void beginCharacter() { *this = Duel{}; }

    [[nodiscard]] bool pending() const { return pending_; }
    [[nodiscard]] uint64_t arbiter() const { return arbiter_; }
    [[nodiscard]] const std::string& challengerName() const { return challengerName_; }
    /// Seconds left of the countdown, or zero when none is running.
    [[nodiscard]] float countdownRemaining(Clock::time_point now) const;

private:
    bool pending_ = false;
    uint64_t arbiter_ = 0;
    std::string challengerName_;
    uint32_t countdownMs_ = 0;
    Clock::time_point countdownStartedAt_{};
};

} // namespace wowee::duel
