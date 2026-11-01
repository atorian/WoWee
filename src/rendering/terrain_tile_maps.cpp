#include "rendering/terrain_tile_maps.hpp"

#include "pipeline/terrain_mesh.hpp"

#include <algorithm>
#include <cstddef>

namespace wowee::rendering {

namespace {

constexpr size_t kCoverTexels = TerrainTileMapPixels::kCoverSize * TerrainTileMapPixels::kCoverSize;
constexpr size_t kNormalTexels = TerrainTileMapPixels::kNormalSize * TerrainTileMapPixels::kNormalSize;

/// Straight up, as the renderer's flat normal: what a chunk with no normals
/// is lit from.
constexpr uint8_t kFlatNormal[4] = {128, 128, 255, 128};

}  // namespace

TerrainTileMapPixels terrainTileMapPixels(const pipeline::TerrainMesh& mesh) {
    TerrainTileMapPixels out;
    out.cover.assign(TerrainChunkSlot::kCount * kCoverTexels * 4, 0);
    out.normals.assign(TerrainChunkSlot::kCount * kNormalTexels * 4, 0);

    for (int cy = 0; cy < TerrainChunkSlot::kChunksPerSide; ++cy) {
        for (int cx = 0; cx < TerrainChunkSlot::kChunksPerSide; ++cx) {
            const pipeline::ChunkMesh& chunk = mesh.getChunk(cx, cy);
            if (!chunk.isValid()) continue;
            const uint32_t layer = TerrainChunkSlot::at(cx, cy).layer();

            // Layer 0 is the base, so the three blended layers are 1..3, in r, g, b.
            uint8_t* cover = out.cover.data() + layer * kCoverTexels * 4;
            for (size_t i = 1; i < chunk.layers.size() && i < 4; ++i) {
                const std::vector<uint8_t>& alpha = chunk.layers[i].alphaData;
                const size_t channel = i - 1;
                const size_t given = std::min(alpha.size(), kCoverTexels);
                for (size_t t = 0; t < kCoverTexels; ++t) {
                    cover[t * 4 + channel] = t < given ? alpha[t] : 255;
                }
            }

            uint8_t* normals = out.normals.data() + layer * kNormalTexels * 4;
            if (chunk.normalGrid.size() == kNormalTexels * 4) {
                std::copy(chunk.normalGrid.begin(), chunk.normalGrid.end(), normals);
            } else {
                for (size_t t = 0; t < kNormalTexels; ++t) std::copy_n(kFlatNormal, 4, normals + t * 4);
                ++out.chunksLitFlat;
            }
        }
    }
    return out;
}

}  // namespace wowee::rendering
