#pragma once
#include "collision/movement_input.hpp"
#include "collision/movement_state.hpp"
#include <variant>
#include <deque>

namespace wowee::collision {
enum class MovementMode { Gravity, FeatherFall, WaterWalk, Hover, Root, CanFly, SwimFlyTransition };
// Retire requests the original lifecycle check; it does not assert removal.
struct MovementCommandEffect {
    enum class Kind { Falling=1, Unroot, MoveStopped, FallLand, Retire, StateChanged } kind;
    uint32_t time=0,oldFlags=0,oldFlags2=0;
    bool wasJumping=false;
};
class MovementCommandSink {
public:
    virtual ~MovementCommandSink() = default;
    // Runs after the mover's height has changed. The client owns packet
    // construction; receiving this notification does not mean a packet was sent.
    virtual void collisionHeightChanged(uint32_t timeMs,float height,
        uint32_t ackCounter,bool needAck) = 0;
    virtual void movementEffect(const MovementCommandEffect& effect) = 0;
    virtual bool forcedMovementChanged(uint32_t timeMs,uint32_t opcode,float value,uint32_t ackCounter,bool needAck) = 0;
    virtual bool inputChanged(uint32_t timeMs,uint32_t opcode,bool needAck) = 0;
    virtual void angularInputStopped(bool pitch) = 0;
    // Equivalent to the unit's UpdateHeartbeatTimerA: the owner updates the
    // clock only when this unit is the active mover.
    virtual void resetHeartbeat(uint32_t timeMs) = 0;
};
enum class MovementQueueResult { Idle, Pending, Deferred };

// Offline subset of the original movement event queue: height and basic local
// input, speeds/rates, knockback and server mode toggles without remote heartbeat payloads. Teleports,
// splines and interpolation still require verified dispatch before gameplay.
class MovementCommandQueue {
public:
    void enqueueCollisionHeight(uint32_t timeMs,float height,
        uint32_t ackCounter,bool needAck);
    void enqueueMode(uint32_t timeMs,uint32_t ackCounter,MovementMode mode,bool enabled);
    void enqueueLaunch(uint32_t timeMs,uint32_t ackCounter,const glm::vec2& direction,float speed,float verticalSpeed);
    void enqueueRateChange(uint32_t timeMs,MovementRate rate,float value,uint32_t ackCounter,bool needAck);
    void enqueueInput(uint32_t timeMs,MovementInput input,bool needAck);
    [[nodiscard]] std::optional<uint32_t> nextTime() const;
    [[nodiscard]] size_t size() const { return commands_.size(); }
    [[nodiscard]] MovementQueueResult process(uint32_t timeMs,
        bool vehicleDefersMovement,bool activeMover,SurfaceMovementState& state,
        SurfaceMovementShape& shape,MovementCommandSink& sink);
private:
    struct Height {float height;uint32_t counter;};
    struct Mode {MovementMode mode;bool enabled;uint32_t counter;};
    struct Launch {glm::vec2 direction;float speed,verticalSpeed;uint32_t counter;};
    struct Rate {MovementRate rate;float value;uint32_t counter;};
    struct Command {
        uint32_t timeMs;
        bool needAck;
        std::variant<Height,MovementInput,Rate,Launch,Mode> action;
    };
    void enqueue(Command command);
    std::deque<Command> commands_;
};
} // namespace wowee::collision
