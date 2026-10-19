#include "collision/terrain_heights.hpp"

namespace wowee::collision {

std::shared_ptr<const TerrainTileHeights> TerrainTileHeights::build(
        const pipeline::ADTTerrain& terrain, int tileX, int tileY) {
    auto heights = std::make_shared<TerrainTileHeights>();
    heights->tileX = tileX;
    heights->tileY = tileY;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            const pipeline::MapChunk& source = terrain.getChunk(cx, cy);
            Chunk& chunk = heights->chunks[cy * 16 + cx];
            chunk.position[0] = source.position[0];
            chunk.position[1] = source.position[1];
            chunk.position[2] = source.position[2];
            chunk.heightMap = source.heightMap;
            chunk.holes = source.holes;
            chunk.areaId = source.areaId;
        }
    }
    return heights;
}

}  // namespace wowee::collision
