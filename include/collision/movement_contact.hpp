#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <glm/glm.hpp>

namespace wowee::collision {

// These are collision planes, not authored shading normals. Facets retain
// their input order: the 12340 contact algorithm resolves ties in that order.
struct MovementPlane {
    glm::vec3 normal{0, 0, 1};
    float offset = 0;
};
struct MovementFacet {
    MovementPlane plane;
    std::array<glm::vec3, 3> vertices;
    uint64_t owner = 0;
};
struct MovementBody {
    glm::vec3 feet;
    float radius;
    float height;

    // Dimensions belong to the mover's model/scale and server state, not a
    // universal player-sized default in the collision library.
    MovementBody(glm::vec3 position, float bodyRadius, float bodyHeight)
        : feet(position), radius(bodyRadius), height(bodyHeight) {}
};
struct MovementContact {
    float distance = 0;
    // facets.size() means no hit. A contact can occur at the requested end.
    uint32_t facet = 0;
    std::array<MovementPlane, 9> planes{};
    uint32_t planeCount = 0;
};

// 12340 DistanceToMove (0x75f9d0), with explicit immutable facet inputs.
// direction is normalized, distance is nonnegative; geometry and dimensions
// are finite. The caller owns facet availability and simulation transitions.
// This primitive clips a body's travel; it does not choose steps or grounding.
[[nodiscard]] MovementContact traceMovementBody(std::span<const MovementFacet> facets,
                                               const MovementBody& body,
                                               const glm::vec3& direction,
                                               float distance);

} // namespace wowee::collision
