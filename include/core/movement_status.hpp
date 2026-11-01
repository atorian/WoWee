#pragma once
#include <cstdint>

namespace wowee {
namespace collision { struct SurfaceMovementState; }
namespace game { struct MovementInfo; }
namespace core {
// Build 12340 GetMoveStatus for ordinary movement messages, without an active
// spline or attached transport. Coordinates are already in original world
// space. Forced transport-change and teleport-ACK formats are separate paths.
// This adapter remains offline until the complete mover is integrated.
[[nodiscard]] game::MovementInfo makeUnattachedMovementStatus12340(
    const collision::SurfaceMovementState& state,uint32_t timeMs);
}
}
