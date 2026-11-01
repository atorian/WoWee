#pragma once

#include "collision/movement_surface.hpp"

namespace wowee::collision {
struct MovementFacetRequest {
    glm::vec3 bodyMin, bodyMax;
    glm::vec3 queryMin, queryMax;
    uint32_t solidFlags;
    bool queryLiquid;
};

// Build 12340 GetMoveFacets query preparation, with no attached transport.
// Input direction is already in original world coordinates. The world owner
// executes solidFlags and, when requested, 0x20000 for liquids; liquid planes
// are reversed before they enter the swimming phase.
[[nodiscard]] MovementFacetRequest makeMovementFacetRequest(
    const SurfaceMovementState& state, const SurfaceMovementShape& shape,
    const glm::vec3& direction, float distance, uint32_t elapsedMs,
    bool locallyControlled, bool ghost);
// ValidateTestVsFacetQuery (0x75f0a0). No request when the displaced body
// remains inside the cached box; otherwise extend it by the original margin.
// The caller replaces its cache only after the world query succeeds.
[[nodiscard]] std::optional<MovementFacetRequest> expandMovementFacetRequest(
    const MovementFacetRequest& cached, const SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec3& displacement,
    bool locallyControlled, bool ghost);
} // namespace wowee::collision
