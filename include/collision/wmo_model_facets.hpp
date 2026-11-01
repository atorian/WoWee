#pragma once
#include "collision/wmo_shape.hpp"
#include "collision/movement_request.hpp"
#include "collision/movement_facet.hpp"

namespace wowee::collision {
class WmoLiquidMaterials {
public:
    virtual ~WmoLiquidMaterials() = default;
    // The owning database reports missing rows once, with the liquid id.
    virtual std::optional<uint32_t> flags(uint16_t type) const = 0;
};
struct WmoModelFacetResult {
    MovementGeometryStatus status = MovementGeometryStatus::Ready;
    uint32_t originalStatusFlags = 0;
    uint32_t degenerateFaces = 0; // The geometry owner reports once per asset.
};
// Model-local part of GetMapObjFacets: GetTris followed by GetLiquidFacets.
// Group selection uses MOGI, in file order. Each group's BSP retains MOGP
// bounds. Missing group readiness is handled by the world placement collector.
// Appends world-space facets, preserving original batch/index pool limits.
[[nodiscard]] WmoModelFacetResult appendWmoModelFacets(
    const WmoShape& model, const glm::vec3& low, const glm::vec3& high,
    uint32_t flags, const glm::mat4& transform, uint64_t owner,
    const WmoLiquidMaterials& materials, FacetNormalization normalization,
    std::vector<MovementFacet>& output);
} // namespace wowee::collision
