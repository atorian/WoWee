#pragma once
#include "collision/movement_surface.hpp"

namespace wowee::collision {
// Shared state operations in build 12340, without an active spline. The event
// dispatcher owns command order, unit effects and ACKs. In particular the
// feather-fall flag changes before the ACK, PostFeatherFall runs afterward.
enum class MovementRate {
    Walk, Run, RunBack, Swim, SwimBack, Flight, FlightBack, Turn, Pitch
};
[[nodiscard]] bool applyMovementRateChange(SurfaceMovementState& state, MovementRate rate, float value);

enum class MovementStateChange {
    Gravity,
    FeatherFall, PostFeatherFall, WaterWalk, Hover, Root, Unroot
};
// Rates/speeds use value verbatim. State toggles use value != 0. Root and
// PostFeatherFall ignore value; Unroot uses it for the original tryFall input.
// Returns the original change result for gravity, true for void
// operations. A true return is not a notification or permission to send an ACK.
[[nodiscard]] bool applyMovementStateChange(SurfaceMovementState& state,
    MovementStateChange change, float value);
} // namespace wowee::collision
