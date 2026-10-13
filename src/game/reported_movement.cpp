#include "game/reported_movement.hpp"
#include "game/movement_flags.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace wowee {
namespace game {

namespace {

constexpr uint32_t bit(MovementFlags f) { return static_cast<uint32_t>(f); }

// The client's fall physics.
constexpr float kGravity = 19.29110527f;
constexpr float kTerminalSpeed = 60.148003f;
constexpr float kFeatherFallTerminalSpeed = 7.0f;

// How quickly a correction is worked off: it shrinks by e every this many
// seconds, so a report half a yard off is drawn away within about a third of
// a second. Further than kSnapDistance is a teleport, not a correction.
constexpr float kCorrectionTime = 0.12f;
constexpr float kSnapDistance = 10.0f;
constexpr float kSettledDistance = 0.01f;

// Playback trails the newest report by about as late as reports have lately
// come, plus a margin: reports late by up to that arrive before they are due.
// A late one raises the delay at once; it eases back down over seconds.
constexpr double kMinDelayMs = 50.0;
constexpr double kMaxDelayMs = 500.0;
constexpr double kDelayMarginMs = 20.0;
constexpr double kDelayEasePerSecondMs = 100.0;

// Behind where it should be, playback speeds up by 1x for every
// kCatchUpMs it is behind, up to kMaxRate; ahead of it, it slows a little.
// Further behind than kMaxBehindMs, the rest is folded in at once rather
// than replayed for seconds.
constexpr double kCatchUpMs = 500.0;
constexpr float kMinRate = 0.8f;
constexpr float kMaxRate = 3.0f;
constexpr double kMaxBehindMs = 1500.0;

// Past the newest report the unit carries on as it last said - that is what
// the half-second between heartbeats is - but no further than this. Then it
// waits for the reports to catch up.
constexpr double kMaxExtrapolationMs = 750.0;

// Simulation is integrated in steps no longer than this, so a turn bends the
// path even when playback covers a lot of it in one frame.
constexpr float kStepSeconds = 0.05f;

// The quickest report fixes how the mover's clock lines up with this one.
// Letting that drift up by a millisecond a second keeps a mover whose clock
// runs slow from looking later and later; crystal clocks drift far less.
constexpr double kClockDriftAllowance = 0.001;

constexpr uint32_t kTravelKeys =
    bit(MovementFlags::FORWARD) | bit(MovementFlags::BACKWARD) |
    bit(MovementFlags::STRAFE_LEFT) | bit(MovementFlags::STRAFE_RIGHT) |
    bit(MovementFlags::ASCENDING) | bit(MovementFlags::DESCENDING);

double millisecondsOf(ReportedMovement::Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
}

float clampFallSpeed(float upSpeed, uint32_t flags) {
    const float terminal = (flags & bit(MovementFlags::FEATHER_FALL))
        ? kFeatherFallTerminalSpeed : kTerminalSpeed;
    return std::max(upSpeed, -terminal);
}

} // namespace

bool ReportedMovement::report(const MovementReport& r, Clock::time_point receivedAt) {
    // Where this report sits on the mover's clock.
    int64_t time = r.moverTime;
    if (haveReport_) {
        const int32_t step = static_cast<int32_t>(r.moverTime - newestMoverTime_);
        if (step < 0) return false;  // Older than one already taken
        time = newestTime_ + step;
        const double sinceMs = std::max(0.0, millisecondsOf(receivedAt) - millisecondsOf(lastReceivedAt_));
        clockOffsetMs_ += kClockDriftAllowance * sinceMs;
        delayMs_ = std::max(kMinDelayMs, delayMs_ - kDelayEasePerSecondMs * sinceMs / 1000.0);
    } else {
        delayMs_ = kMinDelayMs;
    }

    // How late it came, against the quickest report from this mover.
    const double offsetMs = millisecondsOf(receivedAt) - static_cast<double>(time);
    if (!haveReport_ || offsetMs < clockOffsetMs_) clockOffsetMs_ = offsetMs;
    const double latenessMs = offsetMs - clockOffsetMs_;
    delayMs_ = std::clamp(std::max(delayMs_, latenessMs + kDelayMarginMs), kMinDelayMs, kMaxDelayMs);

    newestTime_ = time;
    newestMoverTime_ = r.moverTime;
    lastReceivedAt_ = receivedAt;
    lastLatenessMs_ = static_cast<float>(latenessMs);

    if (!haveReport_) {
        apply(r, time);
        correction_ = glm::vec3(0.0f);
        haveReport_ = true;
    } else {
        pending_.push_back({r, time});
    }
    return true;
}

void ReportedMovement::advance(float dt, Clock::time_point now, const MovementSpeeds& speeds,
                               const MovementCollision* collision) {
    if (!haveReport_ || dt <= 0.0f) return;

    const double targetTime = millisecondsOf(now) - clockOffsetMs_ - delayMs_;
    double behindMs = targetTime - simulatedTime_;
    double playTo = simulatedTime_;
    if (behindMs > kMaxBehindMs) {
        // Too far behind to replay: fold the oldest of it in at once.
        playTo = targetTime - kMaxBehindMs;
        behindMs = kMaxBehindMs;
    }
    const float wantedRate =
        std::clamp(static_cast<float>(1.0 + behindMs / kCatchUpMs), kMinRate, kMaxRate);
    playTo += static_cast<double>(dt) * 1000.0 * wantedRate;
    playTo = std::min(playTo, static_cast<double>(newestTime_) + kMaxExtrapolationMs);
    // What playback actually covers this frame: held at the end of what it
    // knows, it stands still, animation included, until the reports come.
    const double startedAt = simulatedTime_;

    // Each report in its turn. One already behind playback - later than the
    // delay allowed for - is taken from its own moment and carried up to now,
    // and the difference that makes is worked off rather than jumped.
    while (!pending_.empty() && static_cast<double>(pending_.front().time) <= playTo) {
        const Queued next = pending_.front();
        pending_.pop_front();
        if (static_cast<double>(next.time) > simulatedTime_) simulateTo(next.time, speeds, collision);
        const double drawnTime = simulatedTime_;
        const glm::vec3 drawn = position();
        apply(next.report, next.time);
        simulateTo(drawnTime, speeds, collision);
        const glm::vec3 gap = drawn - simulated_;
        correction_ = glm::dot(gap, gap) < kSnapDistance * kSnapDistance ? gap : glm::vec3(0.0f);
    }
    if (playTo > simulatedTime_) simulateTo(playTo, speeds, collision);
    const float coveredRate =
        static_cast<float>(std::max(0.0, playTo - startedAt) / (static_cast<double>(dt) * 1000.0));

    correction_ *= std::exp(-dt / kCorrectionTime);
    if (glm::dot(correction_, correction_) < kSettledDistance * kSettledDistance) {
        correction_ = glm::vec3(0.0f);
    }

    // Held at the end of what it knows, a moving unit waits for reports that
    // are late, and its animation waits with it. A unit that has stopped
    // waits for nothing - no report comes until it moves again - and stands
    // breathing at its own pace. Held at zero, every player that had moved
    // since login froze in the last frame of its stand, mount and all.
    rate_ = isMoving() ? coveredRate : 1.0f;
}

void ReportedMovement::apply(const MovementReport& r, int64_t time) {
    state_ = r;
    simulated_ = r.position;
    simulatedTime_ = static_cast<double>(time);
    orientation_ = r.orientation;
    verticalSpeed_ = isAirborne()
        ? clampFallSpeed(r.jumpUpSpeed - kGravity * r.fallTime, r.flags) : 0.0f;
}

void ReportedMovement::simulateTo(double time, const MovementSpeeds& speeds,
                                  const MovementCollision* collision) {
    while (simulatedTime_ < time) {
        const double stepMs = std::min(time - simulatedTime_, static_cast<double>(kStepSeconds) * 1000.0);
        integrate(static_cast<float>(stepMs / 1000.0), speeds, collision);
        simulatedTime_ += stepMs;
    }
}

void ReportedMovement::integrate(float dt, const MovementSpeeds& speeds,
                                 const MovementCollision* collision) {
    const uint32_t flags = state_.flags;
    const glm::vec3 before = simulated_;

    if (flags & bit(MovementFlags::TURN_LEFT))  orientation_ += speeds.turnRate * dt;
    if (flags & bit(MovementFlags::TURN_RIGHT)) orientation_ -= speeds.turnRate * dt;

    if (isAirborne()) {
        // Committed to the arc it left the ground on: the keys held now do
        // not steer it.
        simulated_.x += state_.jumpCos * state_.jumpXYSpeed * dt;
        simulated_.y += state_.jumpSin * state_.jumpXYSpeed * dt;
        const float upBefore = verticalSpeed_;
        verticalSpeed_ = clampFallSpeed(verticalSpeed_ - kGravity * dt, flags);
        simulated_.z += 0.5f * (upBefore + verticalSpeed_) * dt;
    } else if (isTravelling()) {
        const bool forward  = (flags & bit(MovementFlags::FORWARD)) != 0;
        const bool backward = (flags & bit(MovementFlags::BACKWARD)) != 0;
        const bool left     = (flags & bit(MovementFlags::STRAFE_LEFT)) != 0;
        const bool right    = (flags & bit(MovementFlags::STRAFE_RIGHT)) != 0;
        const bool swimming = (flags & bit(MovementFlags::SWIMMING)) != 0;
        const bool flying   = (flags & bit(MovementFlags::FLYING)) != 0;
        const bool walking  = (flags & bit(MovementFlags::WALKING)) != 0;

        // Swimming and flying go where the unit looks, pitch included.
        const float pitch = (swimming || flying) ? state_.pitch : 0.0f;
        const glm::vec3 ahead(std::cos(orientation_) * std::cos(pitch),
                              std::sin(orientation_) * std::cos(pitch),
                              std::sin(pitch));
        const glm::vec3 leftward(-std::sin(orientation_), std::cos(orientation_), 0.0f);
        glm::vec3 direction = ahead * (float(forward) - float(backward)) +
                              leftward * (float(left) - float(right));
        if (swimming || flying) {
            if (flags & bit(MovementFlags::ASCENDING))  direction.z += 1.0f;
            if (flags & bit(MovementFlags::DESCENDING)) direction.z -= 1.0f;
        }

        const float lengthSq = glm::dot(direction, direction);
        if (lengthSq > 1e-6f) {
            // Backing up is slower in every mode; holding a strafe key as well
            // does not change that.
            const bool backingUp = backward && !forward;
            float speed;
            if (flying)        speed = backingUp ? speeds.flightBack : speeds.flight;
            else if (swimming) speed = backingUp ? speeds.swimBack : speeds.swim;
            else if (walking)  speed = backingUp ? std::min(speeds.walk, speeds.runBack) : speeds.walk;
            else               speed = backingUp ? speeds.runBack : speeds.run;
            simulated_ += direction * (speed * dt / std::sqrt(lengthSq));
        }
    }
    if (collision && simulated_ != before) simulated_ = collision->slide(before, simulated_);
}

bool ReportedMovement::isTravelling() const {
    return (state_.flags & kTravelKeys) != 0 &&
           (state_.flags & bit(MovementFlags::ROOT)) == 0;
}

bool ReportedMovement::isAirborne() const {
    return (state_.flags & (bit(MovementFlags::FALLING) | bit(MovementFlags::FALLINGFAR))) != 0;
}

bool ReportedMovement::isMoving() const {
    return isTravelling() || isAirborne() || !pending_.empty() ||
           glm::dot(correction_, correction_) > 0.0f;
}

} // namespace game
} // namespace wowee
