#include "core/movement_status.hpp"
#include "collision/movement_surface.hpp"
#include "game/world_packet_data.hpp"

namespace wowee::core {
game::MovementInfo makeUnattachedMovementStatus12340(
    const collision::SurfaceMovementState& state,uint32_t time) {
    game::MovementInfo info;
    info.flags=state.flags&~(0x200u|0x8000000u|0x80000000u);
    info.flags2=uint16_t(state.flags2);
    info.time=time;
    info.x=state.position.x;info.y=state.position.y;info.z=state.position.z;
    info.orientation=state.facing;info.pitch=state.pitch;
    info.fallTime=state.fallTimeMs;
    if(info.flags&0x1000) {
        info.jumpVelocity=state.fallVelocity;
        // The original status members have misleading sin/cos names. Its
        // serializer writes horizontal X followed by Y; preserve those bytes.
        info.jumpCosAngle=state.direction2D.x;info.jumpSinAngle=state.direction2D.y;
        info.jumpXYSpeed=state.currentSpeed;
    }
    if(info.flags&0x4000000)info.splineElevation=state.stepStartZ;
    return info;
}
} // namespace wowee::core
