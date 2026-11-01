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
// Build 12340 CMap::CreateImpassableFacets (0x7a43d0). The caller selects
// chunks carrying the impassable flag. Bounds and query use original world
// coordinates. Selection is strictly horizontal; no vertical clipping.
[[nodiscard]] std::vector<MovementFacet> queryImpassableTerrainFacets(
    const glm::vec3& chunkMin, const glm::vec3& chunkMax,
    const glm::vec3& queryMin, const glm::vec3& queryMax);
// Build 12340 CMap::CreateFlightBoundsFacets (0x7a4590). Upper nine
// heights precede lower nine; corner is the map area's authored top-left.
// Original culling truncates Z+/-1 to signed 32 bits, then takes signed low 16.
// Finite query heights must fit int32 after that adjustment.
[[nodiscard]] std::vector<MovementFacet> queryFlightBoundsFacets(
    std::span<const int16_t,18> heights, const glm::vec3& corner,
    float queryMinZ, float queryMaxZ);
// World::AddAaBoxFacets (0x7a3e40), used for unavailable distant geometry.
// Appends twelve facets in original order; nonpositive extents append nothing.
void appendMovementBoxFacets(const glm::vec3& low, const glm::vec3& high,
                             std::vector<MovementFacet>& output);
} // namespace wowee::collision
