#include "collision/movement_execution.hpp"
#include <algorithm>
#include <bit>
#include <iterator>

namespace wowee::collision {
MovementExecutionResult executeUninterpolatedMovement(MovementFacetSource& world,
    MovementRequestCache& cache,SurfaceMovementState& state,SurfaceMovementShape& shape,
    MovementCommandQueue& commands,MovementClock& clock,MovementExecutionSink& sink,
    uint32_t time,uint32_t lastTime,bool active,bool local,bool ghost,bool defer) {
    MovementExecutionResult output;
    auto& movement=output.movement;
    uint32_t cursor=lastTime,total=time-lastTime,processed=0;
    // 0x6f09f0: at most the last 250ms are simulated, even when a command
    // belongs to the skipped interval. Such commands are then due at cursor.
    if(total>250) {
        const uint32_t skipped=total-250;
        cursor=time-250;total=250;
        movement.events.push_back({MovementRequestEvent::Kind::SkipTime,skipped,0,0,0,false});
        if(active)clock.heartbeatTimeMs+=skipped;
    }
    if(total)for(;;) {
        uint32_t step=total-processed;
        if(auto next=commands.nextTime())
            step=std::min(step,uint32_t(std::max(0,std::bit_cast<int32_t>(*next-cursor))));
        if(active&&(state.flags&0xc0100f))step=std::min(step,clock.heartbeatTimeMs-cursor);
        cursor+=step;clock.idleTimeMs=cursor;
        if(step) {
            if(state.flags&0x40c010ff) {
                if(state.flags&0x200)clock.heartbeatTimeMs+=step;
                else {
                    auto part=applyUninterpolatedMovement(world,cache,state,shape,cursor,step,active,local,ghost);
                    movement.elapsedMs+=part.elapsedMs;
                    if(part.geometryStatus!=MovementGeometryStatus::Ready)movement.geometryStatus=part.geometryStatus;
                    if(part.transportRequest)movement.transportRequest=part.transportRequest;
                    if(part.fallResetTime)movement.fallResetTime=part.fallResetTime;
                    if(active)for(const auto& event:part.events)
                        if(event.kind==MovementRequestEvent::Kind::SkipTime)clock.heartbeatTimeMs+=event.time;
                    movement.events.insert(movement.events.end(),std::make_move_iterator(part.events.begin()),std::make_move_iterator(part.events.end()));
                    if(active&&(state.flags&0xc0100f)&&std::bit_cast<int32_t>(cursor-clock.heartbeatTimeMs)>=0)
                        if(sink.heartbeat(cursor))clock.heartbeatTimeMs=cursor+500;
                }
            }
            processed+=step;
        }
        const auto result=commands.process(cursor,defer,state,shape,sink);
        if(result!=MovementQueueResult::Pending) {
            if(result==MovementQueueResult::Idle)output.retireRequested=true;
            else if(active)clock.heartbeatTimeMs+=total-processed;
            break;
        }
        if(processed>=total)break;
    }
    sink.frameFinished(time);
    return output;
}
} // namespace wowee::collision
