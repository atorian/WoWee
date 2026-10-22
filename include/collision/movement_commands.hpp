#pragma once
#include "collision/movement_surface.hpp"
#include <deque>

namespace wowee::collision {
struct CollisionHeightCommand {
    uint32_t timeMs;
    uint32_t ackCounter;
    float height;
    bool needAck;
};
class MovementCommandSink {
public:
    virtual ~MovementCommandSink() = default;
    // Runs after the mover's height has changed. The client owns packet
    // construction; receiving this notification does not mean a packet was sent.
    virtual void collisionHeightChanged(uint32_t timeMs,float height,
        uint32_t ackCounter,bool needAck) = 0;
};
enum class MovementQueueResult { Idle, Pending, Deferred };

// Offline subset of the original movement event queue: collision-height
// commands without remote heartbeat payloads. Input, teleports, splines and
// interpolation require their own verified dispatch before gameplay adoption.
class MovementCommandQueue {
public:
    void enqueueCollisionHeight(uint32_t timeMs,float height,
        uint32_t ackCounter,bool needAck);
    [[nodiscard]] std::optional<uint32_t> nextTime() const;
    [[nodiscard]] size_t size() const { return heights_.size(); }
    [[nodiscard]] MovementQueueResult process(uint32_t timeMs,
        bool vehicleDefersMovement,SurfaceMovementState& state,
        SurfaceMovementShape& shape,MovementCommandSink& sink);
private:
    std::deque<CollisionHeightCommand> heights_;
};
} // namespace wowee::collision
