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
// CChunkLiquid::CreateVertXY (0x7cdf80). Original material vertex formats
// above 1 have height zero; they do not use the visual min/max-height fallback.
// Chunk indices are global ADT chunk coordinates in [0,1023].
[[nodiscard]] std::vector<glm::vec3> makeTerrainLiquidVertices(
    int chunkX, int chunkY, TerrainFacetCells bounds, float baseZ,
    std::span<const float> heights, uint16_t vertexFormat);
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
// A WMO liquid uses positive model-space X/Y grid coordinates and its own
// triangle winding. Vertices come from GenLiquidVerts, not the water renderer.
struct WmoLiquidGrid {
    uint32_t xVerts, yVerts, xTiles, yTiles;
    glm::vec3 corner;
    std::span<const glm::vec3> vertices;
    std::span<const uint8_t> tiles;
    uint16_t liquidType;
    uint32_t liquidFlags;
};
[[nodiscard]] std::vector<glm::vec3> makeWmoLiquidVertices(
    uint32_t xVerts, uint32_t yVerts, const glm::vec3& corner,
    std::span<const float> heights);
// CMapObjGroup::GetLiquidFacets (0x7ca110), with the model-local query box.
// A valid grid has x/yVerts=x/yTiles+1 and at most 65536 vertices.
[[nodiscard]] std::optional<LiquidFacetQuery> queryWmoLiquidFacets(
    const WmoLiquidGrid& grid, const glm::vec3& queryMin, const glm::vec3& queryMax,
    uint32_t flags, const glm::mat4& transform, uint64_t owner,
    FacetNormalization normalization);
} // namespace wowee::collision
