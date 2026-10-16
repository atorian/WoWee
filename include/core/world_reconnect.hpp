#pragma once

#include "game/world_login.hpp"

#include <cstdint>
#include <string>

namespace wowee {
namespace game { class GameHandler; }

namespace core {

/// How the client renews the session it lost without asking the player: the
/// login server is shown the last session's key (no password) and asked where
/// the realm is now. Application's is LoginServerRenewal; a test's is its own.
class WorldSessionRenewal {
public:
    enum class Result {
        Pending,
        /// login() is what to connect to the world server with.
        Renewed,
        /// The login server said no; asking again gets the same answer.
        Refused,
        /// The login server could not be reached; it may be there later.
        Unreachable,
    };

    virtual ~WorldSessionRenewal() = default;
    /// Start renewing the session `last` was made with.
    virtual void begin(const game::WorldLogin& last) = 0;
    /// Each frame until it is decided.
    virtual Result poll(float deltaTime) = 0;
    [[nodiscard]] virtual game::WorldLogin login() const = 0;
    /// Why it was refused or not reached.
    [[nodiscard]] virtual std::string failure() const = 0;
    /// Stop what is under way.
    virtual void cancel() = 0;
};

/// The attempts to log a character back in after the world connection was
/// lost in the world, while the client keeps the world it was in.
///
/// Each attempt renews the session, connects to the world server with the
/// last login at the realm's current address, waits for the character list
/// and logs the same character in - the game's own login path, which resets
/// what the character held exactly as a login from the character screen
/// does. An attempt that fails is followed by the next after 5 s times its
/// number; after the last, or at a refusal, it gives up. Being back in the
/// world is said by the game (WorldEntered), not here.
class WorldReconnect {
public:
    static constexpr int kMaxAttempts = 5;

    enum class Status { Reconnecting, GaveUp };

    /// Begins the first attempt on the first update. `game` has already let go
    /// of the lost connection; its last login is what is renewed.
    WorldReconnect(game::GameHandler& game, WorldSessionRenewal& renewal, uint64_t characterGuid);
    ~WorldReconnect();

    WorldReconnect(const WorldReconnect&) = delete;
    WorldReconnect& operator=(const WorldReconnect&) = delete;

    Status update(float deltaTime);

    [[nodiscard]] int attempt() const { return attempt_; }
    /// Seconds since the connection was lost.
    [[nodiscard]] float elapsed() const { return elapsed_; }
    /// Why it gave up.
    [[nodiscard]] const std::string& failure() const { return failure_; }
    /// What the player is told over the world.
    [[nodiscard]] std::string statusText() const;

private:
    enum class Step { Waiting, Renewing, Connecting, Entering, GaveUp };

    void startAttempt();
    void enterStep(Step step);
    void attemptFailed(const std::string& why);
    void giveUp(const std::string& why);

    game::GameHandler& game_;
    WorldSessionRenewal& renewal_;
    const uint64_t characterGuid_;
    const game::WorldLogin lastLogin_;
    Step step_ = Step::Waiting;
    int attempt_ = 0;
    float waitLeft_ = 0.0f;
    float stepTime_ = 0.0f;
    float elapsed_ = 0.0f;
    std::string lastError_;
    std::string failure_;
};

} // namespace core
} // namespace wowee
