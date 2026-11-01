#pragma once

#include "collision/movement_contact.hpp"

namespace wowee::collision::detail {
// Shared original triangle/quad extrusion. Keep contact and footprint tests
// on the same numerical implementation, including x87 intermediate spills.
[[nodiscard]] bool extrudeMovementFace(std::span<const glm::vec3> vertices,
    std::span<const uint8_t> face, const glm::vec3& move,
    std::span<MovementPlane> planes);
// Lower four body faces only, used by the original hover support query.
[[nodiscard]] MovementContact traceMovementPyramid(std::span<const MovementFacet> facets,
    const MovementBody& body, const glm::vec3& direction, float distance);
}
