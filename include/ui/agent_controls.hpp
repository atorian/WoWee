#pragma once

/**
 * agent_controls.hpp - an agent driving the character, when the player says so.
 *
 * The MCP endpoint's movement tools land here. Like the gamepad, this says what
 * it means through the channels the client already listens on - core::Input's
 * virtual keys for walking and jumping, set by the application from the frame
 * this hands back, and the camera controller's look delta for turning - so the
 * movement chain, its animations and the packets that announce it run exactly
 * as they do for a keyboard.
 *
 * Nothing moves unless the player handed control over (Ctrl+Shift+G or
 * /ai on), and any input of the player's own takes it straight back: a key,
 * a click, the wheel, or Ctrl+Shift+H.
 */

#include <array>

namespace wowee {
namespace rendering { class CameraController; }
namespace ui {

class AgentControls {
public:
    enum class Move { Forward, Backward, StrafeLeft, StrafeRight };

    /// What one frame asks of the client.
    struct Frame {
        std::array<bool, 4> held{};   ///< indexed by Move
        bool jump = false;            ///< pressed this frame only
        float turnDegrees = 0.0f;     ///< facing and view together; positive turns right
        float lookYawDegrees = 0.0f;  ///< the view alone; positive turns right
        float lookPitchDegrees = 0.0f;///< positive looks up
        float zoomNotches = 0.0f;     ///< positive moves the camera in
        /// Whether to set the keys at all: while the agent has control, and
        /// once more on the frame it loses it, to let go of them.
        bool setKeys = false;
    };

    [[nodiscard]] bool hasControl() const { return control_; }
    void grant();
    void release();
    /// Any key, click or wheel of the player's own.
    void onPlayerInput();

    // Each refuses, returning false, while the agent has no control.
    bool move(Move direction, float seconds);
    bool turn(float degrees);
    bool look(float yawDegrees, float pitchDegrees);
    bool zoom(float notches);
    bool jump();
    void stop();

    /// Counts the held moves down by deltaTime and hands out what is queued.
    [[nodiscard]] Frame advance(float deltaTime);

    /// advance(), with the turn, look and zoom applied to the camera. The keys
    /// are the caller's to set: the input belongs to the application.
    Frame update(float deltaTime, rendering::CameraController* camera);
    /// Whether the character should face where the view does this frame.
    [[nodiscard]] bool isSteering() const { return steering_; }

    /// The on-screen notice while the agent has control.
    void drawIndicator() const;

private:
    bool control_ = false;
    bool clearKeys_ = false;
    bool steering_ = false;
    std::array<float, 4> remaining_{};
    std::array<bool, 4> fresh_{};
    Frame pending_;
};

} // namespace ui
} // namespace wowee
