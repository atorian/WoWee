#include "core/world_reconnect.hpp"

#include "core/logger.hpp"
#include "game/game_handler.hpp"

#include <algorithm>

namespace wowee::core {

namespace {

// How long each step may take before the attempt is given up on. The world
// server greets a connection within a second; a login queue (AUTH_SENT) has no
// deadline, as it has none at the character screen either.
constexpr float kRenewDeadline = 20.0f;
constexpr float kGreetingDeadline = 15.0f;
constexpr float kCharacterListDeadline = 60.0f;
constexpr float kEnterDeadline = 30.0f;
constexpr float kWaitPerAttempt = 5.0f;

} // namespace

WorldReconnect::WorldReconnect(game::GameHandler& game, WorldSessionRenewal& renewal,
                               uint64_t characterGuid)
    : game_(game), renewal_(renewal), characterGuid_(characterGuid),
      lastLogin_(game.lastWorldLogin()) {}

WorldReconnect::~WorldReconnect() {
    if (step_ == Step::Renewing) renewal_.cancel();
}

void WorldReconnect::enterStep(Step step) {
    step_ = step;
    stepTime_ = 0.0f;
}

void WorldReconnect::startAttempt() {
    ++attempt_;
    LOG_WARNING("Reconnecting to the world server: attempt ", attempt_, " of ", kMaxAttempts,
                ", ", elapsed_, " s after the connection was lost");
    enterStep(Step::Renewing);
    renewal_.begin(lastLogin_);
}

void WorldReconnect::attemptFailed(const std::string& why) {
    LOG_WARNING("Reconnect attempt ", attempt_, " of ", kMaxAttempts, " failed: ", why);
    lastError_ = why;
    if (step_ == Step::Renewing) renewal_.cancel();
    // What the attempt reached of the world server goes, as a logout's does.
    game_.disconnect();
    if (attempt_ >= kMaxAttempts) {
        giveUp("could not reconnect after " + std::to_string(kMaxAttempts) + " attempts: " + why);
        return;
    }
    enterStep(Step::Waiting);
    waitLeft_ = kWaitPerAttempt * static_cast<float>(attempt_);
}

void WorldReconnect::giveUp(const std::string& why) {
    if (step_ == Step::Renewing) renewal_.cancel();
    enterStep(Step::GaveUp);
    failure_ = why;
    LOG_ERROR("Reconnecting gave up after ", elapsed_, " s: ", why,
              "; the character leaves the world for the login screen");
}

WorldReconnect::Status WorldReconnect::update(float deltaTime) {
    if (step_ == Step::GaveUp) return Status::GaveUp;
    elapsed_ += deltaTime;
    stepTime_ += deltaTime;
    using game::WorldState;
    switch (step_) {
        case Step::Waiting:
            waitLeft_ -= deltaTime;
            if (waitLeft_ <= 0.0f) startAttempt();
            break;
        case Step::Renewing:
            switch (renewal_.poll(deltaTime)) {
                case WorldSessionRenewal::Result::Pending:
                    if (stepTime_ > kRenewDeadline) attemptFailed("the login server did not answer");
                    break;
                case WorldSessionRenewal::Result::Renewed:
                    enterStep(Step::Connecting);
                    if (!game_.connect(renewal_.login()))
                        attemptFailed("the connection to the world server could not be started");
                    break;
                case WorldSessionRenewal::Result::Refused:
                    giveUp(renewal_.failure());
                    break;
                case WorldSessionRenewal::Result::Unreachable:
                    attemptFailed(renewal_.failure());
                    break;
            }
            break;
        case Step::Connecting: {
            const WorldState state = game_.getState();
            if (state == WorldState::CHAR_LIST_RECEIVED) {
                const auto& characters = game_.getCharacters();
                const bool listed = std::any_of(characters.begin(), characters.end(),
                    [this](const game::Character& c) { return c.guid == characterGuid_; });
                if (!listed) {
                    giveUp("the character is no longer on the realm's character list");
                    break;
                }
                enterStep(Step::Entering);
                game_.selectCharacter(characterGuid_);
            } else if (state == WorldState::FAILED) {
                // Said in full by the game's own failure line, just before.
                attemptFailed("the world server could not be reached or refused the login");
            } else if (state == WorldState::DISCONNECTED) {
                attemptFailed("the world server closed the connection");
            } else if (state == WorldState::CONNECTING || state == WorldState::CONNECTED ||
                       state == WorldState::CHALLENGE_RECEIVED) {
                if (stepTime_ > kGreetingDeadline) attemptFailed("the world server did not answer");
            } else if (state != WorldState::AUTH_SENT && stepTime_ > kCharacterListDeadline) {
                attemptFailed("no character list after a minute");
            }
            break;
        }
        case Step::Entering: {
            const WorldState state = game_.getState();
            if (state == WorldState::FAILED || state == WorldState::DISCONNECTED) {
                attemptFailed("the world server closed the connection while entering the world");
            } else if (state == WorldState::CHAR_LIST_RECEIVED) {
                // SMSG_CHARACTER_LOGIN_FAILED puts the list back.
                attemptFailed("the world server refused the character's login");
            } else if (state != WorldState::IN_WORLD && stepTime_ > kEnterDeadline) {
                attemptFailed("the world server did not take the character into the world");
            }
            break;
        }
        case Step::GaveUp:
            break;
    }
    return step_ == Step::GaveUp ? Status::GaveUp : Status::Reconnecting;
}

std::string WorldReconnect::statusText() const {
    std::string text = "Connection lost. Reconnecting";
    if (attempt_ > 0) {
        text += " (attempt " + std::to_string(attempt_) + " of " + std::to_string(kMaxAttempts) + ")";
    }
    text += "...";
    if (step_ == Step::Waiting && attempt_ > 0) {
        text += "\n" + lastError_ + ". Trying again in " +
                std::to_string(static_cast<int>(std::max(0.0f, waitLeft_) + 0.99f)) + " s.";
    }
    return text;
}

} // namespace wowee::core
