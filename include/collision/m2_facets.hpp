#pragma once
#include "collision/movement_contact.hpp"
#include <vector>

namespace wowee::collision {
// Original CM2Model::GetCollisionFacets (0x82ec30). Inputs are valid authored
// vertices, indices and one normal per collision face, in original coordinates.
// Face count comes from normals; callers validate every referenced index.
[[nodiscard]] std::vector<MovementFacet> queryM2Facets(
    std::span<const glm::vec3> vertices, std::span<const uint16_t> indices,
    std::span<const glm::vec3> normals, const glm::mat4& transform,
    const glm::vec3& queryMin, const glm::vec3& queryMax, uint64_t owner);
}
