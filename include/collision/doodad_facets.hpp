#pragma once
#include "collision/m2_shape.hpp"
#include "collision/movement_request.hpp"
#include <span>

namespace wowee::collision {
struct DoodadFacetModel {
    // Shared M2 bounds, independent of the collision mesh's bounds. A present
    // model may legitimately have no authored collision triangles.
    glm::vec3 boundsMin{},boundsMax{};
    const M2Shape* shape = nullptr;
};
struct DoodadFacetPlacement {
    uint32_t flags = 0,staticFlags = 0,queryTag = 0;
    uint64_t owner = 0;
    const DoodadFacetModel* model = nullptr;
    glm::vec3 boundsMin{},boundsMax{};
    glm::mat4 transform{1.0f};
};
// CMap::GetDoodadDefFacets (0x7a50c0). Links retain the original list order and
// contain non-null pointers and may reference the same placement from several chunks/groups. One query tag
// spans those lists. The collision owner serializes this mutable query state.
void appendDoodadFacets(std::span<DoodadFacetPlacement* const> links,
    const MovementFacetRequest& request,uint32_t flags,uint32_t queryTag,
    std::vector<MovementFacet>& output);
} // namespace wowee::collision
