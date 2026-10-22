#include "collision/movement_commands.hpp"
#include "collision/movement_dimensions.hpp"
#include <bit>

namespace wowee::collision {
void MovementCommandQueue::enqueueCollisionHeight(uint32_t time,float height,
    uint32_t counter,bool needAck) {
    enqueue({time,needAck,Height{height,needAck?counter:0}});
}
void MovementCommandQueue::enqueueRateChange(uint32_t time,MovementRate rate,float value,
    uint32_t counter,bool needAck) {
    enqueue({time,needAck,Rate{rate,value,needAck?counter:0}});
}
void MovementCommandQueue::enqueueInput(uint32_t time,MovementInput input,bool needAck) {
    enqueue({time,needAck,input});
}
void MovementCommandQueue::enqueue(Command command) {
    // 0x6ec090: stable insertion using signed differences of wrapping clocks.
    // A numeric sort is wrong across rollover; std::sort also requires an
    // ordering relation that modular timestamps do not provide for all inputs.
    auto at=commands_.begin();
    for(;at!=commands_.end();++at)
        if(std::bit_cast<int32_t>(command.timeMs-at->timeMs)<0)break;
    commands_.insert(at,command);
}
std::optional<uint32_t> MovementCommandQueue::nextTime() const {
    if(commands_.empty())return std::nullopt;
    return commands_.front().timeMs;
}
MovementQueueResult MovementCommandQueue::process(uint32_t time,bool defer,
    SurfaceMovementState& state,SurfaceMovementShape& shape,MovementCommandSink& sink) {
    // UpdatePlayerMovement 0x6ef860. A deferred passenger keeps the due event
    // queued; future events alone do not trigger the deferral result.
    while(!commands_.empty()&&std::bit_cast<int32_t>(time-commands_.front().timeMs)>=0) {
        if(defer)return MovementQueueResult::Deferred;
        const auto event=commands_.front();
        commands_.pop_front();
        state.moveStartDelayMs=0;
        const uint32_t oldFlags=state.flags;
        bool sent=false;
        if(const auto* height=std::get_if<Height>(&event.action)) {
            setMovementCollisionHeight(shape,height->height);
            sink.collisionHeightChanged(time,height->height,height->counter,event.needAck);
        } else if(const auto* rate=std::get_if<Rate>(&event.action)) {
            // State changes precede notification, even if the value compares
            // equal and no anchor update is needed. No basic-input root gate.
            (void)applyMovementRateChange(state,rate->rate,rate->value);
            constexpr uint32_t opcodes[]={0x2db,0xe3,0xe5,0xe7,0x2dd,0x382,0x384,0x2df,0x45d};
            sent=sink.rateChanged(time,opcodes[static_cast<unsigned>(rate->rate)],rate->value,rate->counter,event.needAck);
        } else {
            const auto input=std::get<MovementInput>(event.action);
            const bool movement=input<=MovementInput::StopStrafe||input==MovementInput::Jump;
            if(movement&&(oldFlags&(0x200|0x800|0x100000)))continue;
            const bool changed=applyMovementInput(state,input);
            const bool conditional=input==MovementInput::Ascend||input==MovementInput::Descend||
                input==MovementInput::StopAscend||input==MovementInput::Jump;
            if(changed||!conditional) {
                constexpr uint32_t opcodes[]={0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbc,0xbd,
                    0xbe,0xbf,0xc0,0xc1,0x359,0x3a7,0x35a,0xbb};
                sent=sink.inputChanged(time,opcodes[static_cast<unsigned>(input)],event.needAck);
            }
            if(input==MovementInput::StopTurn||input==MovementInput::StopPitch)
                sink.angularInputStopped(input==MovementInput::StopPitch);
        }
        if(event.needAck&&!sent&&(state.flags&0xc0100f)&&!(oldFlags&0xc0100f))
            sink.resetHeartbeat(time);
        // SetInterpolation 0x6ea6a0 clears all three interpolation bits; the
        // next event in this subset has no remote heartbeat to interpolate to.
        state.flags2&=~0x1c00u;
    }
    return !commands_.empty()||(state.flags&0xc010ff)?MovementQueueResult::Pending:MovementQueueResult::Idle;
}
} // namespace wowee::collision
