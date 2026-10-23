#include "collision/movement_frame.hpp"
#include "collision/movement_plot.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

namespace wowee::collision {
namespace {
// 0x6e9f50. Remote extrapolation ramps down after 500ms. Arithmetic and
// signed tests are on the original wrapping 32-bit counters.
uint32_t plotTime(SurfaceMovementState& s,uint32_t elapsed,bool active) {
    if(active||!(s.flags&0xc0100f))return s.anchorElapsedMs;
    s.moveStartDelayMs+=elapsed;
    const uint32_t delay=s.moveStartDelayMs;
    if(delay<=500)return s.anchorElapsedMs;
    const int32_t lag=std::max(0,std::bit_cast<int32_t>(delay-1500));
    const int32_t ramp=std::min(1000,std::bit_cast<int32_t>(delay-500));
    const int32_t time=std::max(0,std::bit_cast<int32_t>(s.anchorElapsedMs-uint32_t(lag)));
    const int32_t reduction=std::bit_cast<int32_t>(uint32_t(ramp)*uint32_t(ramp))/2000;
    return uint32_t(std::max(0,std::bit_cast<int32_t>(uint32_t(time)-uint32_t(reduction))));
}
bool validPosition(const glm::vec3& p) {
    const double x=17066.666f-double(p.x),y=17066.666f-double(p.y);
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&
        x>=2&&y>=2&&x<double(34133.332f)-2&&y<double(34133.332f)-2;
}
}
// 0x6eac40 -> PlotUnitMovement -> RequestMove -> CollideRequestMove.
MovementRequestResult applyUninterpolatedMovement(MovementFacetSource& world,
    MovementRequestCache& cache,SurfaceMovementState& state,const SurfaceMovementShape& shape,
    uint32_t time,uint32_t elapsed,bool active,bool local,bool ghost,MovementPendingObserver* pendingObserver) {
    MovementRequestResult result;
    if(!validPosition(state.position)||!elapsed)return result;
    while(result.elapsedMs<elapsed) {
        if(!(state.flags&0xc010ff)&&(!(state.flags&0x40000000)||(state.flags&0x800)))break;
        const uint32_t step=elapsed-result.elapsedMs;
        state.anchorElapsedMs+=step;
        glm::vec3 move{0};
        if(!plotMovement(state,plotTime(state,step,active),move,&state.facing,&state.pitch)&&!(state.flags&0x40000000))return result;
        // RequestMove stores the target but retains the x87 values when it
        // subtracts the current position; reloading the stores changes queries.
        const glm::vec3 requested{
            (double(move.x)+state.anchorPosition.x)-state.position.x,
            (double(state.anchorPosition.y)+move.y)-state.position.y,
            (double(state.anchorPosition.z)+move.z)-state.position.z};
        auto part=traceUnattachedMovementRequest(world,cache,state,shape,requested,time-step,step,local,ghost,pendingObserver);
        result.elapsedMs+=part.elapsedMs;
        if(part.geometryStatus!=MovementGeometryStatus::Ready)result.geometryStatus=part.geometryStatus;
        if(part.transportRequest)result.transportRequest=part.transportRequest;
        if(part.fallResetTime)result.fallResetTime=part.fallResetTime;
        result.events.insert(result.events.end(),std::make_move_iterator(part.events.begin()),std::make_move_iterator(part.events.end()));
    }
    return result;
}
} // namespace wowee::collision
