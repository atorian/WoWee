#pragma once
#include "collision/movement_surface.hpp"

namespace wowee::collision {
enum class MovementInput {
    Forward,Backward,Stop,StrafeLeft,StrafeRight,StopStrafe,
    TurnLeft,TurnRight,StopTurn,PitchUp,PitchDown,StopPitch,
    Ascend,Descend,StopAscend,Jump
};
// Shared movement-state operations, without an attached transport or spline.
// Return the original operation's success value; void turn/pitch starts return
// true. Queue admission, notification decisions and packets belong to dispatch.
[[nodiscard]] bool applyMovementInput(SurfaceMovementState& state,MovementInput input);
} // namespace wowee::collision
