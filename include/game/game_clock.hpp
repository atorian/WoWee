#pragma once

#include <chrono>
#include <cmath>

namespace wowee {
namespace game {

/**
 * The realm's time of day, kept running between the packets that set it.
 *
 * The server sends the hour and minute once - SMSG_LOGIN_SETTIMESPEED at login,
 * SMSG_GAMETIME_SET/UPDATE or SMSG_GAMESPEED_SET if it ever changes them - and
 * leaves the client to advance it. Storing that hour and reading it back froze
 * the sky at whatever time the character logged in.
 *
 * The speed is in game minutes per real second. Servers send 1/60, which is
 * real time: one game minute per real minute, one game day per real day.
 */
class GameClock {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr float kRealTimeSpeed = 1.0f / 60.0f;

    /// The server says it is `hours` (0-24) now, running at `speed`.
    void set(float hours, float speed, Clock::time_point at = Clock::now()) {
        speed_ = (std::isfinite(speed) && speed > 0.0f) ? speed : kRealTimeSpeed;
        setHours(hours, at);
    }

    /// The server says it is `hours` now; the speed it set earlier stands.
    void setHours(float hours, Clock::time_point at = Clock::now()) {
        anchorHours_ = wrap(hours);
        anchorAt_ = at;
        known_ = true;
    }

    [[nodiscard]] bool known() const { return known_; }
    [[nodiscard]] float speed() const { return speed_; }

    /// Hours since midnight at `at`, or -1 before the server has said.
    [[nodiscard]] float hours(Clock::time_point at = Clock::now()) const {
        if (!known_) return -1.0f;
        const double realSeconds = std::chrono::duration<double>(at - anchorAt_).count();
        const double gameHours = realSeconds * static_cast<double>(speed_) / 60.0;
        return wrap(static_cast<float>(std::fmod(anchorHours_ + gameHours, 24.0)));
    }

private:
    static float wrap(float h) {
        h = std::fmod(h, 24.0f);
        return h < 0.0f ? h + 24.0f : h;
    }

    float anchorHours_ = 0.0f;
    Clock::time_point anchorAt_{};
    float speed_ = kRealTimeSpeed;
    bool known_ = false;
};

}  // namespace game
}  // namespace wowee
