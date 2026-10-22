#pragma once
#include "collision/movement_surface.hpp"

namespace wowee::collision {
// Original PlotUnitMovement: displacement from the anchor at elapsed time.
// Facing/pitch outputs are optional and change only for their active inputs.
// Modes without translation preserve the supplied displacement. The caller's
// frame dispatcher owns time slicing, anchors, collision and event delivery.
uint32_t plotMovement(const SurfaceMovementState& state,uint32_t elapsedMs,
    glm::vec3& displacement,float* facing,float* pitch);
} // namespace wowee::collision
