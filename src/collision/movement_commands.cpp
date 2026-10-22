#include "collision/movement_commands.hpp"
#include "collision/movement_dimensions.hpp"
#include <bit>
#include <algorithm>

namespace wowee::collision {
void MovementCommandQueue::enqueueCollisionHeight(uint32_t time,float height,
    uint32_t counter,bool needAck) {
    enqueue({time,needAck,Height{height,needAck?counter:0}});
}
void MovementCommandQueue::enqueueMode(uint32_t time,uint32_t counter,MovementMode mode,bool enabled) {
    enqueue({time,true,Mode{mode,enabled,counter}});
}
void MovementCommandQueue::enqueueLaunch(uint32_t time,uint32_t counter,const glm::vec2& direction,
    float speed,float verticalSpeed) {
    enqueue({time,true,Launch{direction,speed,verticalSpeed,counter}});
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
MovementQueueResult MovementCommandQueue::process(uint32_t time,bool defer,bool active,
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
        } else if(const auto* mode=std::get_if<Mode>(&event.action)) {
            const bool enable=mode->enabled;
            const auto change=[&](MovementStateChange kind,float value=0) {
                return applyMovementStateChange(state,kind,value);
            };
            const auto notify=[&](uint32_t opcode,float value=0) {
                return sink.forcedMovementChanged(time,opcode,value,mode->counter,true);
            };
            using Effect=MovementCommandEffect;
            switch(mode->mode) {
            case MovementMode::Gravity:
                if(change(MovementStateChange::Gravity,enable)) {
                    if((state.flags2&4)||(state.flags&0x400)) {
                        const uint32_t before=state.flags&~0x400u;
                        const bool jumping=(before&0x1000)&&state.fallVelocity!=0;
                        change(MovementStateChange::StopFalling);
                        const bool queuedStart=std::any_of(commands_.begin(),commands_.end(),[](const Command& c) {
                            const auto* input=std::get_if<MovementInput>(&c.action);
                            return input&&(*input==MovementInput::Forward||*input==MovementInput::Backward);
                        });
                        struct Pending final:MovementPendingObserver {
                            MovementCommandSink& sink;bool notify;
                            Pending(MovementCommandSink& s,bool n):sink(s),notify(n){}
                            void forwardStopped() override {if(notify)sink.movementEffect({Effect::Kind::MoveStopped});}
                        } pending(sink,active&&!queuedStart);
                        (void)applyPendingMovementActions(state,pending);
                        if(!(state.flags&0x1000)&&(before&0x1000))
                            sink.movementEffect({Effect::Kind::FallLand,0,before,0,jumping});
                        sink.movementEffect({Effect::Kind::Retire});
                    } else change(MovementStateChange::TryStartFalling);
                }
                sent=notify(enable?0x4d1:0x4cf);
                break;
            case MovementMode::FeatherFall:
                change(MovementStateChange::FeatherFall,enable);
                sent=notify(0x2cf,enable);
                change(MovementStateChange::PostFeatherFall);
                sink.movementEffect({Effect::Kind::Falling});
                break;
            case MovementMode::WaterWalk:
                change(MovementStateChange::WaterWalk,enable);
                if(!enable&&active)change(MovementStateChange::TryStartFalling);
                sent=notify(0x2d0,enable);
                break;
            case MovementMode::Hover:
                if(!enable&&active) {
                    const bool started=change(MovementStateChange::TryStartFalling);
                    if((state.flags&0x1000)&&!(state.flags&0x2000)) {
                        state.flags|=0x2000;
                        sink.movementEffect({Effect::Kind::Falling});
                    } else if(started)sink.movementEffect({Effect::Kind::Falling});
                }
                change(MovementStateChange::Hover,enable);
                sent=notify(0xf6,enable);
                break;
            case MovementMode::Root: {
                const uint32_t flags=state.flags,flags2=state.flags2;
                const bool jumping=(flags&0x1000)&&state.fallVelocity!=0;
                change(enable?MovementStateChange::Root:MovementStateChange::Unroot,active);
                if(!enable)sink.movementEffect({Effect::Kind::Unroot});
                sent=notify(enable?0xe9:0xeb);
                sink.movementEffect({Effect::Kind::StateChanged,time,flags,flags2,jumping});
                break;
            }
            case MovementMode::CanFly:
                if(!enable&&(state.flags&0x2000000))change(MovementStateChange::StopFlying);
                state.flags=enable?state.flags|0x1000000u:state.flags&~0x1000000u;
                (void)notify(0x345,enable); // Original dispatcher ignores this return value.
                break;
            case MovementMode::SwimFlyTransition:
                state.flags2=enable?state.flags2|0x4000u:state.flags2&~0x4000u;
                (void)notify(0x340,enable);
                break;
            }
        } else if(const auto* launch=std::get_if<Launch>(&event.action)) {
            if(oldFlags&0x200)continue; // Original queue drops knockback on a transport.
            launchUnattachedMovement(state,launch->direction,launch->speed,launch->verticalSpeed);
            sent=sink.forcedMovementChanged(time,0xf0,0,launch->counter,true);
        } else if(const auto* rate=std::get_if<Rate>(&event.action)) {
            // State changes precede notification, even if the value compares
            // equal and no anchor update is needed. No basic-input root gate.
            (void)applyMovementRateChange(state,rate->rate,rate->value);
            constexpr uint32_t opcodes[]={0x2db,0xe3,0xe5,0xe7,0x2dd,0x382,0x384,0x2df,0x45d};
            sent=sink.forcedMovementChanged(time,opcodes[static_cast<unsigned>(rate->rate)],rate->value,rate->counter,event.needAck);
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
