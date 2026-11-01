#include "collision/movement_dimensions.hpp"
#include <cmath>

namespace wowee::collision {
void setMovementModelDimensions(SurfaceMovementShape& shape,float width,float height,
    float scale,float referenceScale,bool force) {
    shape.stepHeight=std::fabs(referenceScale)<2.3841858e-7f?1.0f:float(double(scale)/referenceScale);
    shape.radius=float((double(width)*scale)*.5);
    if (force || !shape.clientControlled) shape.height=float(double(scale)*height);
}
void setMovementCollisionHeight(SurfaceMovementShape& shape,float height) {
    shape.height=height;
}
} // namespace wowee::collision
