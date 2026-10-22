#pragma once
#include "collision/m2_shape.hpp"
#include "collision/movement_request.hpp"

namespace wowee::collision {
struct EntityFacetPlacement {
    uint32_t staticFlags = 0,queryTag = 0,parameter = 0;
    uint64_t owner = 0;
};
struct EntityFacetData {
    const M2Shape* shape = nullptr; // A valid model may have no collision mesh.
    glm::vec3 boundsMin{},boundsMax{};
    glm::mat4 transform{1.0f};
};
class EntityFacetProvider {
public:
    virtual ~EntityFacetProvider() = default;
    // Absence means the original callback returned false. Providers report
    // actual asset failures, with their identity, before returning no geometry.
    virtual std::optional<EntityFacetData> query(uint64_t& owner,uint32_t parameter,uint32_t flags) = 0;
};
// CMap::QueryDestructibleFacets (0x7a5240). The first link without bit 2 ends
// this list; hidden/visited links merely skip. Tags are shared across lists.
// Every link points to a live, collision-owner-managed placement.
void appendEntityFacets(std::span<EntityFacetPlacement* const> links,
    EntityFacetProvider* provider,const MovementFacetRequest& request,
    uint32_t flags,uint32_t queryTag,std::vector<MovementFacet>& output);
} // namespace wowee::collision
