#pragma once

#include "pipeline/adt_loader.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <glm/glm.hpp>

namespace wowee::collision {

/// The ground of one map tile as the world's queries read it: each of its 256
/// chunks' height grid, holes and area, where the chunk is.
///
/// Taken from the parsed tile on the terrain worker, beside the mesh, and kept
/// by CollisionWorld for as long as the tile is loaded. Immutable once built.
/// The rest of the parsed tile - texture layers, alpha maps, clutter - is the
/// renderer's.
struct TerrainTileHeights {
    /// The tile's index the way TerrainManager keys it: x from render Y,
    /// y from render X (core::coords::worldToTile).
    int tileX = 0;
    int tileY = 0;
    bool collisionDataValid = true;
    std::optional<std::array<int16_t,18>> flightBounds;
    std::vector<pipeline::ADTTerrain::DoodadPlacement> doodadPlacements;
    std::vector<pipeline::ADTTerrain::WMOPlacement> wmoPlacements;

    struct Chunk {
        float position[3] = {};
        pipeline::HeightMap heightMap;
        uint16_t holes = 0;
        uint32_t areaId = 0;
        bool loaded = false;
        glm::vec3 originalCorner{}, boundsMin{}, boundsMax{};
        uint32_t flags = 0;
        std::vector<uint32_t> doodadReferences, wmoReferences;
        std::vector<pipeline::ADTTerrain::CollisionLiquidLayer> liquids;

        [[nodiscard]] bool hasHeights() const { return heightMap.isLoaded(); }
        /// Whether quad (y, x), 0..7 each, is a hole, in the bit order MCNK
        /// keeps them in - the order the mesh builder skips them in too.
        [[nodiscard]] bool isHole(int y, int x) const {
            const int bit = 1 << ((y / 2) * 4 + (x / 2));
            return (bit & holes) != 0;
        }
    };
    /// Chunk (x, y) at [y * 16 + x], as ADTTerrain keeps them.
    std::array<Chunk, 256> chunks;

    [[nodiscard]] uint64_t heapBytes() const;

    /// The ground and authored collision metadata of a parsed tile.
    [[nodiscard]] static std::shared_ptr<const TerrainTileHeights> build(
        const pipeline::ADTTerrain& terrain, int tileX, int tileY);
};

}  // namespace wowee::collision
