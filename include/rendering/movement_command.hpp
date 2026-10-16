#pragma once

#include <cstdint>

namespace wowee::rendering {

/// What the player is asking the character to do, by command rather than by
/// key: which key that is belongs to the key bindings. The original client's
/// bindings.xml drives every one of these from a binding script -
/// MOVEFORWARD's is MoveForwardStart() on the press and MoveForwardStop() on
/// the release - so a key the account gives to something else stops moving
/// the character, and a key the account gives to these starts.
enum class MovementCommand : uint8_t {
    Forward,
    Backward,
    TurnLeft,
    TurnRight,
    StrafeLeft,
    StrafeRight,
    /// Jump on the ground, rise while swimming or flying.
    JumpOrAscend,
    /// Sit down or stand up, dive while swimming or flying.
    SitOrDescend,
    Count
};

/// Who is holding a command. A command is held while any of them holds it, so
/// the on-screen stick letting go does not stop a key that is still down.
enum class MovementSource : uint8_t {
    /// The key bindings: a keyboard key or a pad button, through the
    /// interface's binding scripts.
    Bindings,
    /// A controller's left stick.
    PadStick,
    /// The on-screen stick a touch screen draws.
    TouchStick,
};

[[nodiscard]] constexpr const char* movementCommandName(MovementCommand command) {
    switch (command) {
        case MovementCommand::Forward:      return "Forward";
        case MovementCommand::Backward:     return "Backward";
        case MovementCommand::TurnLeft:     return "TurnLeft";
        case MovementCommand::TurnRight:    return "TurnRight";
        case MovementCommand::StrafeLeft:   return "StrafeLeft";
        case MovementCommand::StrafeRight:  return "StrafeRight";
        case MovementCommand::JumpOrAscend: return "JumpOrAscend";
        case MovementCommand::SitOrDescend: return "SitOrDescend";
        case MovementCommand::Count:        break;
    }
    return "?";
}

} // namespace wowee::rendering
