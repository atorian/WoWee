#include "duel/domain/duel.hpp"

#include <utility>

namespace wowee::duel {

namespace {
constexpr uint32_t kLongestCountdownMs = 30000;
constexpr uint32_t kUsualCountdownMs = 3000;
} // namespace

std::vector<DuelChange> Duel::challenged(uint64_t arbiter, uint64_t challenger, uint64_t self,
                                         std::string challengerName) {
    arbiter_ = arbiter;
    if (challenger == self) {
        pending_ = false;
        return {};
    }
    challengerName_ = std::move(challengerName);
    pending_ = true;
    return {DuelRequested{challengerName_}};
}

void Duel::countdownStarted(uint32_t ms, Clock::time_point now) {
    countdownMs_ = (ms > 0 && ms <= kLongestCountdownMs) ? ms : kUsualCountdownMs;
    countdownStartedAt_ = now;
}

std::vector<DuelChange> Duel::completed(bool started) {
    pending_ = false;
    countdownMs_ = 0;
    return {DuelFinished{started}};
}

float Duel::countdownRemaining(Clock::time_point now) const {
    if (countdownMs_ == 0) return 0.0f;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - countdownStartedAt_).count();
    const float rem = (static_cast<float>(countdownMs_) - static_cast<float>(elapsed)) / 1000.0f;
    return rem > 0.0f ? rem : 0.0f;
}

} // namespace wowee::duel
