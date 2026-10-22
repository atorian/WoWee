#pragma once

#include "collision/movement_facet.hpp"
#include <span>
#include <vector>

namespace wowee::collision {
// Inclusive cells in an 8x8 terrain chunk, in authored MCNK column/row order.
struct TerrainFacetCells { int minX, minY, maxX, maxY; };

// Build 12340 CMapChunk::Intersect (0x7d8840). The box is chunk-local; corner
// places returned facets in original world coordinates. Heights retain the
// authored interleaved 9/8-row layout. The caller owns chunk availability and
// supplies valid cell indices in [0,7]. Nullopt means an unsupported CPU path.
[[nodiscard]] std::optional<std::vector<MovementFacet>> queryTerrainFacets(
    std::span<const float, 145> heights, uint16_t holes, const glm::vec3& corner,
    TerrainFacetCells cells, const glm::vec3& queryMin, const glm::vec3& queryMax,
    FacetNormalization normalization);
} // namespace wowee::collision
