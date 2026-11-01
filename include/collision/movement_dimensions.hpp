#pragma once
#include "collision/movement_surface.hpp"

namespace wowee::collision {
// CMovement_C::SetCollisionBox (0x6e9570). Client-controlled movers keep their
// authoritative height on model changes unless force is set. Radius and step
// scale still change. Inputs retain the original model/reference scales.
void setMovementModelDimensions(SurfaceMovementShape& shape,float width,float height,
    float scale,float referenceScale,bool force);
// CMovement_C::SetCollisionHeight (0x6e9600). Applied at the movement event,
// before its acknowledgement; callers must not substitute model height later.
void setMovementCollisionHeight(SurfaceMovementShape& shape,float height);
} // namespace wowee::collision
