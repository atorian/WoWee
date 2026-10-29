#pragma once

#include <cstdint>
#include <vector>

namespace wowee::pipeline {
struct TerrainMesh;
}

namespace wowee::rendering {

/// Where a chunk's maps are in its tile's: the layer of the tile's arrays
/// (TerrainTileMaps) that holds what its layers cover and how it is lit.
/// Made from the chunk's place in its tile, so no two chunks of a tile share
/// one and every chunk finds its own.
class TerrainChunkSlot {
public:
    static constexpr int kChunksPerSide = 16;
    static constexpr uint32_t kCount = kChunksPerSide * kChunksPerSide;

    /// Unplaced: the first layer, until the chunk is given its own.
    TerrainChunkSlot() = default;

    /// The slot of the chunk at (chunkX, chunkY) of its tile, each 0 to 15.
    [[nodiscard]] static TerrainChunkSlot at(int chunkX, int chunkY) {
        return TerrainChunkSlot(static_cast<uint32_t>(chunkY * kChunksPerSide + chunkX));
    }

    [[nodiscard]] uint32_t layer() const { return layer_; }

private:
    explicit TerrainChunkSlot(uint32_t layer) : layer_(layer) {}
    uint32_t layer_ = 0;
};

/// A tile's chunk maps as its arrays hold them, every chunk at its slot's
/// layer: what TerrainTileMaps is uploaded from.
struct TerrainTileMapPixels {
    static constexpr uint32_t kCoverSize = 64;
    static constexpr uint32_t kNormalSize = 17;

    /// RGBA8, TerrainChunkSlot::kCount layers of kCoverSize squared: how much
    /// of the chunk each of its three blended layers covers, the first in r,
    /// the second in g and the third in b. A layer with no alpha map covers
    /// all of it, and an alpha map short of 64x64 covers the rest.
    std::vector<uint8_t> cover;
    /// RGBA8, TerrainChunkSlot::kCount layers of kNormalSize squared: the
    /// chunk's normals (ChunkMesh::normalGrid), or straight up where it has none.
    std::vector<uint8_t> normals;
    /// Chunks drawn from the mesh whose normals were missing or the wrong size.
    int chunksLitFlat = 0;
};

[[nodiscard]] TerrainTileMapPixels terrainTileMapPixels(const pipeline::TerrainMesh& mesh);

}  // namespace wowee::rendering
