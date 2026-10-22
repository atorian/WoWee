#pragma once
#include "collision/wmo_model_facets.hpp"
#include <span>

namespace wowee::collision {
struct WmoWorldGroup {
    uint32_t index = 0;
    WmoGroupBounds bounds{};
    uint64_t link = 0; // Collision owner's stable group-link identity.
};
struct WmoWorldPlacement {
    uint32_t flags = 0;
    const WmoShape* model = nullptr;
    bool groupsReady = false;
    WmoGroupBounds bounds{};
    glm::mat4 transform{1.0f}, inverse{1.0f};
    uint64_t owner = 0;
    // Original linked-group order, independent of render visibility.
    std::span<const WmoWorldGroup> groups;
};
struct DestructibleCollisionProxy { glm::vec3 center{}; float radius = 0; };
class WmoLinkedFacets {
public:
    virtual ~WmoLinkedFacets() = default;
    virtual MovementGeometryStatus doodads(uint64_t link, const MovementFacetRequest& request,
        uint32_t flags, std::vector<MovementFacet>& output) = 0;
    virtual MovementGeometryStatus destructibles(uint64_t link, const MovementFacetRequest& request,
        uint32_t flags, std::vector<MovementFacet>& output) = 0;
};
// CMap::GetMapObjFacets (0x7a55e0). Placements/proxies must have original list
// order. Appends collected facets even if a later placement is not ready.
// Linked M2/entity collection is a required service, not an empty fallback.
[[nodiscard]] WmoModelFacetResult appendWorldWmoFacets(
    std::span<const WmoWorldPlacement> placements,
    std::span<const DestructibleCollisionProxy> proxies,
    const MovementFacetRequest& request, uint32_t flags,
    const WmoLiquidMaterials& materials, WmoLinkedFacets& linked,
    FacetNormalization normalization, std::vector<MovementFacet>& output);
} // namespace wowee::collision
