#pragma once
#include "collision/terrain_heights.hpp"
#include "collision/movement_request.hpp"
#include "collision/terrain_facets.hpp"

namespace wowee::collision {
struct TerrainLiquidMaterial { uint32_t flags; uint16_t vertexFormat; };
class TerrainLiquidMaterials {
public:
    virtual ~TerrainLiquidMaterials() = default;
    // Missing required data is a load failure, not a default water material.
    // The material owner reports a missing id once at warning/error level.
    virtual std::optional<TerrainLiquidMaterial> find(uint16_t liquidType) const = 0;
};
// The loaded terrain/liquid part of GetChunkFacets (0x7a5a60). The world
// checks area availability first and appends this chunk's doodads/entities
// afterward. Output is appended in original order, including impassable walls.
[[nodiscard]] MovementGeometryStatus appendTerrainChunkFacets(
    const TerrainTileHeights::Chunk& chunk, int globalX, int globalY, uint32_t mapId,
    TerrainFacetCells globalCells, const MovementFacetRequest& request, uint32_t flags,
    const TerrainLiquidMaterials& materials, FacetNormalization normalization,
    std::vector<MovementFacet>& output);
} // namespace wowee::collision
