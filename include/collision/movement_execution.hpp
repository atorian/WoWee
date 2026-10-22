#pragma once
#include "collision/movement_commands.hpp"
#include "collision/movement_frame.hpp"

namespace wowee::collision {
struct MovementClock {
    uint32_t idleTimeMs=0;
    uint32_t heartbeatTimeMs=0;
};
class MovementExecutionSink:public MovementCommandSink {
public:
    // Unit/network boundary. True means the unit accepted the heartbeat and
    // advanced its reporting clock; packet encoding is outside this interface.
    virtual bool heartbeat(uint32_t timeMs)=0;
    virtual void frameFinished(uint32_t timeMs)=0;
};
struct MovementExecutionResult {
    MovementRequestResult movement;
    // The owner must perform original RemoveFromMoversList checks (including
    // hover support and spline completion). This is a request, not retirement.
    bool retireRequested=false;
};
// Original ExecuteMovement scheduling over the verified uninterpolated frame
// path and basic-input/height-command subset. No attached transport or active spline.
// Unit state-event callbacks remain observations, as in MovementRequestResult.
// This offline composition is not a gameplay/network replacement yet.
[[nodiscard]] MovementExecutionResult executeUninterpolatedMovement(
    MovementFacetSource& world,MovementRequestCache& cache,SurfaceMovementState& state,
    SurfaceMovementShape& shape,MovementCommandQueue& commands,MovementClock& clock,
    MovementExecutionSink& sink,uint32_t timeMs,uint32_t lastTimeMs,
    bool activeMover,bool locallyControlled,bool ghost,bool vehicleDefersMovement);
} // namespace wowee::collision
