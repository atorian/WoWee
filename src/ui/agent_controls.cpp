#include "ui/agent_controls.hpp"

#include <algorithm>

namespace wowee {
namespace ui {

void AgentControls::grant() { control_ = true; }

void AgentControls::release() {
    if (control_) clearKeys_ = true;
    control_ = false;
    stop();
    pending_ = Frame{};
}

void AgentControls::onPlayerInput() { release(); }

bool AgentControls::move(Move direction, float seconds) {
    if (!control_ || seconds <= 0.0f) return false;
    const auto index = static_cast<size_t>(direction);
    remaining_[index] = std::max(remaining_[index], seconds);
    fresh_[index] = true;
    return true;
}

bool AgentControls::turn(float degrees) {
    if (!control_) return false;
    pending_.turnDegrees += degrees;
    return true;
}

bool AgentControls::look(float yawDegrees, float pitchDegrees) {
    if (!control_) return false;
    pending_.lookYawDegrees += yawDegrees;
    pending_.lookPitchDegrees += pitchDegrees;
    return true;
}

bool AgentControls::zoom(float notches) {
    if (!control_) return false;
    pending_.zoomNotches += notches;
    return true;
}

bool AgentControls::jump() {
    if (!control_) return false;
    pending_.jump = true;
    return true;
}

void AgentControls::stop() {
    remaining_.fill(0.0f);
    fresh_.fill(false);
}

AgentControls::Frame AgentControls::advance(float deltaTime) {
    Frame frame = pending_;
    pending_ = Frame{};
    frame.setKeys = control_ || clearKeys_;
    clearKeys_ = false;
    // deltaTime has already passed since the last frame, so it comes off first;
    // a move asked for since then is held for at least this frame, however short.
    for (size_t i = 0; i < remaining_.size(); ++i) {
        remaining_[i] = std::max(0.0f, remaining_[i] - deltaTime);
        frame.held[i] = fresh_[i] || remaining_[i] > 0.0f;
        fresh_[i] = false;
    }
    steering_ = frame.turnDegrees != 0.0f;
    return frame;
}

} // namespace ui
} // namespace wowee
