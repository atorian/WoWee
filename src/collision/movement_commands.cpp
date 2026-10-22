#include "collision/movement_commands.hpp"
#include "collision/movement_dimensions.hpp"
#include <bit>

namespace wowee::collision {
void MovementCommandQueue::enqueueCollisionHeight(uint32_t time,float height,
    uint32_t counter,bool needAck) {
    // 0x6ec090: stable insertion using signed differences of wrapping clocks.
    // A numeric sort is wrong across rollover; std::sort also requires an
    // ordering relation that modular timestamps do not provide for all inputs.
    auto at=heights_.begin();
    for(;at!=heights_.end();++at)
        if(std::bit_cast<int32_t>(time-at->timeMs)<0)break;
    heights_.insert(at,{time,needAck?counter:0,height,needAck});
}
std::optional<uint32_t> MovementCommandQueue::nextTime() const {
    if(heights_.empty())return std::nullopt;
    return heights_.front().timeMs;
}
MovementQueueResult MovementCommandQueue::process(uint32_t time,bool defer,
    SurfaceMovementState& state,SurfaceMovementShape& shape,MovementCommandSink& sink) {
    // UpdatePlayerMovement 0x6ef860. A deferred passenger keeps the due event
    // queued; future events alone do not trigger the deferral result.
    while(!heights_.empty()&&std::bit_cast<int32_t>(time-heights_.front().timeMs)>=0) {
        if(defer)return MovementQueueResult::Deferred;
        const auto event=heights_.front();
        heights_.pop_front();
        state.moveStartDelayMs=0;
        setMovementCollisionHeight(shape,event.height);
        sink.collisionHeightChanged(time,event.height,event.ackCounter,event.needAck);
        // SetInterpolation 0x6ea6a0 clears all three interpolation bits; the
        // next event in this subset has no remote heartbeat to interpolate to.
        state.flags2&=~0x1c00u;
    }
    return !heights_.empty()||(state.flags&0xc010ff)?MovementQueueResult::Pending:MovementQueueResult::Idle;
}
} // namespace wowee::collision
