#include "collision/terrain_heights.hpp"
#include "pipeline/heap_bytes.hpp"
#include <algorithm>

namespace wowee::collision {

std::shared_ptr<const TerrainTileHeights> TerrainTileHeights::build(
        const pipeline::ADTTerrain& terrain, int tileX, int tileY) {
    auto heights = std::make_shared<TerrainTileHeights>();
    heights->tileX = tileX;
    heights->tileY = tileY;
    heights->collisionDataValid = terrain.collisionDataValid;
    heights->flightBounds = terrain.flightBounds;
    heights->doodadPlacements = terrain.doodadPlacements;
    heights->wmoPlacements = terrain.wmoPlacements;
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
            chunk.loaded = source.loaded;
            chunk.flags = source.flags;
            chunk.doodadReferences = source.doodadReferences;
            chunk.wmoReferences = source.wmoReferences;
            chunk.liquids = terrain.collisionLiquids[cy*16+cx];
            const int globalX=tileX*16+cx,globalY=tileY*16+cy;
            const auto edge=[](int index) { return float(17066.666f-double(index)*33.333332f); };
            chunk.originalCorner={edge(globalY),edge(globalX),source.position[2]};
            chunk.boundsMin={edge(globalY+1),edge(globalX+1),source.position[2]};
            chunk.boundsMax=chunk.originalCorner;
            if (source.hasHeightMap()) {
                const auto [low,high]=std::minmax_element(source.heightMap.heights.begin(),source.heightMap.heights.end());
                chunk.boundsMin.z=float(double(*low)+source.position[2]);
                chunk.boundsMax.z=float(double(*high)+source.position[2]);
            }
        }
    }
    return heights;
}

uint64_t TerrainTileHeights::heapBytes() const {
    using pipeline::vectorBytes;
    uint64_t total=vectorBytes(doodadPlacements)+vectorBytes(wmoPlacements);
    for (const auto& chunk : chunks) {
        total+=vectorBytes(chunk.doodadReferences)+vectorBytes(chunk.wmoReferences);
        total+=vectorBytes(chunk.liquids,[](const pipeline::ADTTerrain::CollisionLiquidLayer& liquid) {
            return vectorBytes(liquid.heights);
        });
    }
    return total;
}

}  // namespace wowee::collision
