#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <glm/vec3.hpp>

namespace wowee {
namespace game {

/// How fast a unit moves in each of its movement modes, in yards per second
/// (the turn rate in radians per second). The server sends them in a unit's
/// movement block and again whenever one changes. The defaults are the
/// client's base speeds, for a unit nothing has been heard about yet.
struct MovementSpeeds {
    float walk       = 2.5f;
    float run        = 7.0f;
    float runBack    = 4.5f;
    float swim       = 4.722222f;
    float swimBack   = 2.5f;
    float flight     = 7.0f;
    float flightBack = 4.5f;
    float turnRate   = 3.141594f;
};

/// The world's walls as a mover meets them. The game knows where units are
/// and where they are going; the geometry they run into lives with the
/// renderer, which implements this.
class MovementCollision {
public:
    virtual ~MovementCollision() = default;
    /// Where a mover going straight from `from` to `to` ends up, slid along
    /// whatever it runs into. Server coordinates.
    [[nodiscard]] virtual glm::vec3 slide(const glm::vec3& from, const glm::vec3& to) const = 0;
};

/// One MSG_MOVE_* report of a unit's movement state, in server coordinates:
/// where it is, which way it faces, which movement keys it holds, and how far
/// into a jump or fall it is.
struct MovementReport {
    uint32_t moverTime = 0;      // The mover's own clock when it sent this, ms
    glm::vec3 position{0.0f};
    float orientation = 0.0f;
    float pitch = 0.0f;          // Swimming and flying: up is positive
    uint32_t flags = 0;          // MovementFlags
    float fallTime = 0.0f;       // Seconds since the jump or fall began
    float jumpUpSpeed = 0.0f;    // Upward speed the jump began with; 0 for a fall
    float jumpCos = 0.0f;        // Horizontal direction of a jump, server axes
    float jumpSin = 0.0f;
    float jumpXYSpeed = 0.0f;    // Horizontal speed of a jump
};

/// The movement of a unit that moves itself: another player, or anything a
/// player controls, played back from the reports it sends.
///
/// Such a unit reports its movement state in MSG_MOVE_* packets - on every
/// change, and every half second while it moves - each stamped with the
/// mover's own clock. The reports are played back on that clock, in order:
/// between two of them the unit is carried on the way the client moves it,
/// along the keys it holds at its speed for that mode, turning at its turn
/// rate, and on a ballistic arc while it is in the air.
///
/// Reports do not arrive evenly. A busy server relays them in bursts, half a
/// second and more late, several at once. So playback runs a little behind
/// the newest report, by about as much as reports have lately been late,
/// and never more than a short way past it. When a burst leaves it further
/// behind it plays faster - every turn and jump still in its place - until it
/// has caught up; playbackRate() says how much faster, for the animation.
/// A report too late even for that is folded in at once, and the gap between
/// where the unit was drawn and where it really is is worked off over a
/// fraction of a second instead of jumped.
///
/// Between reports a unit runs into walls as it would itself - a player
/// holding forward against one stays at it rather than passing through until
/// the next report pulls it back. Height on the ground is not simulated; the
/// renderer stands a ground mover on the floor under it.
class ReportedMovement {
public:
    using Clock = std::chrono::steady_clock;

    /// How a report's stamp sat against the ones before it.
    enum class ReportOrder {
        InOrder,
        /// Stamped before the newest report taken. It arrived after it, and the
        /// connection keeps the order things were sent in, so it is taken as
        /// the newest at the newest one's moment: servers re-stamp relayed
        /// movement with their own clock, and a stop stamped a little early is
        /// still the stop.
        StampedEarlier,
    };

    /// Take a report that reached this client at receivedAt.
    ReportOrder report(const MovementReport& r, Clock::time_point receivedAt);

    /// Play the movement on to now, dt seconds after the last call, against
    /// the walls collision knows (none if null).
    void advance(float dt, Clock::time_point now, const MovementSpeeds& speeds,
                 const MovementCollision* collision = nullptr);

    /// Where the unit is drawn, in server coordinates.
    [[nodiscard]] glm::vec3 position() const { return simulated_ + correction_; }
    [[nodiscard]] float orientation() const { return orientation_; }
    /// The movement flags of the report being played.
    [[nodiscard]] uint32_t flags() const { return state_.flags; }
    /// How fast playback ran against real time last frame: 1 when keeping
    /// up, more while catching up after a burst, 0 while it waits for
    /// reports that are late.
    [[nodiscard]] float playbackRate() const { return rate_; }
    /// How far behind the newest report playback is held, seconds.
    [[nodiscard]] float playbackDelay() const { return static_cast<float>(delayMs_ / 1000.0); }
    /// The moment on the mover's clock being drawn, ms, unwrapped.
    [[nodiscard]] double playbackTime() const { return simulatedTime_; }
    /// How much later than the quickest report the last one arrived, seconds.
    [[nodiscard]] float lastReportLateness() const { return lastLatenessMs_ / 1000.0f; }

    /// A movement key is held - forward, back, either strafe, or up or down
    /// while swimming or flying - and the unit is not rooted.
    [[nodiscard]] bool isTravelling() const;
    /// In a jump or a fall.
    [[nodiscard]] bool isAirborne() const;
    /// The air time began with a jump rather than a step off an edge.
    [[nodiscard]] bool isJump() const { return isAirborne() && state_.jumpUpSpeed > 0.0f; }
    /// Moving at all: travelling, airborne, reports still to play, or a
    /// correction still being worked off.
    [[nodiscard]] bool isMoving() const;

private:
    struct Queued {
        MovementReport report;
        int64_t time = 0;   // Unwrapped mover time
    };

    /// Make r the state being played, at mover time `time`.
    void apply(const MovementReport& r, int64_t time);
    /// Carry the simulated state on to mover time `time`.
    void simulateTo(double time, const MovementSpeeds& speeds, const MovementCollision* collision);
    /// Move the simulated state on by dt seconds.
    void integrate(float dt, const MovementSpeeds& speeds, const MovementCollision* collision);

    MovementReport state_;
    double simulatedTime_ = 0.0;     // Mover time simulated_ stands at
    glm::vec3 simulated_{0.0f};
    glm::vec3 correction_{0.0f};
    float orientation_ = 0.0f;
    float verticalSpeed_ = 0.0f;     // Airborne only, up positive
    std::deque<Queued> pending_;     // Taken, not yet played, oldest first
    bool haveReport_ = false;

    // The mover's clock against this one. moverTime is 32 bits of
    // milliseconds and wraps; it is unwrapped against the newest report.
    int64_t newestTime_ = 0;
    uint32_t newestMoverTime_ = 0;
    double clockOffsetMs_ = 0.0;     // Local ms minus mover ms, quickest seen
    double delayMs_ = 0.0;           // How far playback trails the mover
    Clock::time_point lastReceivedAt_{};
    float lastLatenessMs_ = 0.0f;
    float rate_ = 1.0f;
};

} // namespace game
} // namespace wowee
