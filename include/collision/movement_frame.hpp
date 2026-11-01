#pragma once
#include "collision/movement_request.hpp"

namespace wowee::collision {
// Original ApplyMovement for a mover without a spline, attached transport or
// interpolation flags (0x1c00). Local/remote anchor timing is retained; the
// caller executes queued input events between these intervals. This offline
// boundary does not send packets or claim the full ExecuteMovement contract.
[[nodiscard]] MovementRequestResult applyUninterpolatedMovement(
    MovementFacetSource& world,MovementRequestCache& cache,SurfaceMovementState& state,
    const SurfaceMovementShape& shape,uint32_t timeMs,uint32_t elapsedMs,
    bool activeMover,bool locallyControlled,bool ghost,MovementPendingObserver* pendingObserver=nullptr);
} // namespace wowee::collision
