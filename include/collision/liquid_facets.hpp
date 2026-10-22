#pragma once

#include "collision/terrain_facets.hpp"

namespace wowee::collision {
// One authored CChunkLiquid grid. Bounds are cell coordinates, with exclusive
// maxima; vertices are packed rows, (width+1)*(height+1), in chunk-local space.
// MH2O uses one bit per cell, packed over this rectangle. MCLQ supplies the
// original 8x8 tile bytes instead; its low type nibble determines participation.
struct TerrainLiquidGrid {
    TerrainFacetCells bounds;
    std::span<const glm::vec3> vertices;
    uint64_t cellMask = 0;
    std::span<const uint8_t> legacyTiles;
    uint32_t liquidType = 0;
};
struct LiquidFacetQuery {
    std::vector<MovementFacet> facets;
    uint32_t degenerateFaces = 0; // Geometry owner logs once per asset.
};

// CChunkLiquid::GetTris + CMap::GetChunkLiquidFacets in build 12340. Query
// cells are inclusive, unlike grid bounds. Only Z is tested against the box;
// horizontal selection was already done by the original world cell query.
// Inputs must describe a valid grid inside [0,8], at most 81 finite vertices.
[[nodiscard]] std::optional<LiquidFacetQuery> queryTerrainLiquidFacets(
    const TerrainLiquidGrid& grid, TerrainFacetCells cells,
    const glm::vec3& corner, float queryMinZ, float queryMaxZ,
    FacetNormalization normalization);
} // namespace wowee::collision
