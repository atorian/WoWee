#pragma once

#include "collision/movement_contact.hpp"
#include <optional>

namespace wowee::collision {
// Build 12340 selects these implementations with its CPU capability flag.
// NativeSse uses this CPU's RSQRTSS approximation, not an exact square root.
enum class FacetNormalization { X87, NativeSse };
struct MovementFacetBuild {
    MovementFacet facet;
    // Original keeps this face with an upward normal and reports bad data.
    // The geometry owner must log once per asset, not on every world query.
    bool degenerate = false;
};

// World::TriDataToFacetData (0x782740), one ordered face. Vertices and matrix
// are finite, in original model/world coordinates; do not feed shading normals
// or a render-space inverse-transpose normal matrix. Nullopt means this build
// cannot execute the requested CPU path; it is not an empty collision face.
[[nodiscard]] std::optional<MovementFacetBuild> makeMovementFacet(
    const std::array<glm::vec3, 3>& vertices, const glm::mat4& transform,
    uint64_t owner, FacetNormalization normalization);
} // namespace wowee::collision
