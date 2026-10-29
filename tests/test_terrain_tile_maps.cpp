// A terrain tile's chunk maps as the GPU arrays hold them: every chunk's
// cover and normals at the layer its slot names, and nobody else's there. A
// chunk reading another chunk's layer draws that chunk's blend over its own
// ground; a lost "covers everything" draws a layer as a hole.
#include <catch_amalgamated.hpp>
#include "rendering/terrain_tile_maps.hpp"
#include "pipeline/terrain_mesh.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using wowee::pipeline::ChunkMesh;
using wowee::pipeline::TerrainMesh;
using wowee::rendering::TerrainChunkSlot;
using wowee::rendering::TerrainTileMapPixels;
using wowee::rendering::terrainTileMapPixels;

namespace {

constexpr size_t kCoverTexels = 64 * 64;
constexpr size_t kNormalTexels = 17 * 17;

// A chunk drawn from the mesh: one vertex and one index make it count.
void makeDrawn(ChunkMesh& chunk) {
    chunk.vertices.resize(1);
    chunk.indices.resize(1);
}

ChunkMesh::LayerInfo layerCovering(uint8_t value, size_t texels = kCoverTexels) {
    ChunkMesh::LayerInfo layer{};
    layer.alphaData.assign(texels, value);
    return layer;
}

// The cover of texel `t` at `slot`, as r, g, b.
std::vector<int> coverAt(const TerrainTileMapPixels& pixels, TerrainChunkSlot slot, size_t t) {
    const size_t at = (slot.layer() * kCoverTexels + t) * 4;
    return {pixels.cover[at], pixels.cover[at + 1], pixels.cover[at + 2]};
}

std::vector<int> normalAt(const TerrainTileMapPixels& pixels, TerrainChunkSlot slot, size_t t) {
    const size_t at = (slot.layer() * kNormalTexels + t) * 4;
    return {pixels.normals[at], pixels.normals[at + 1], pixels.normals[at + 2], pixels.normals[at + 3]};
}

}  // namespace

TEST_CASE("Each chunk's layers cover from its own slot, one layer to a channel", "[terrain][maps]") {
    auto mesh = std::make_unique<TerrainMesh>();
    ChunkMesh& a = mesh->getChunk(3, 5);
    makeDrawn(a);
    a.layers = {ChunkMesh::LayerInfo{}, layerCovering(10), layerCovering(20), layerCovering(30)};
    ChunkMesh& b = mesh->getChunk(5, 3);
    makeDrawn(b);
    b.layers = {ChunkMesh::LayerInfo{}, layerCovering(200)};

    const TerrainTileMapPixels pixels = terrainTileMapPixels(*mesh);

    REQUIRE(coverAt(pixels, TerrainChunkSlot::at(3, 5), 0) == std::vector<int>{10, 20, 30});
    REQUIRE(coverAt(pixels, TerrainChunkSlot::at(3, 5), kCoverTexels - 1) == std::vector<int>{10, 20, 30});
    REQUIRE(coverAt(pixels, TerrainChunkSlot::at(5, 3), 0)[0] == 200);
}

TEST_CASE("A layer with no alpha map covers the whole chunk", "[terrain][maps]") {
    auto mesh = std::make_unique<TerrainMesh>();
    ChunkMesh& chunk = mesh->getChunk(0, 0);
    makeDrawn(chunk);
    chunk.layers = {ChunkMesh::LayerInfo{}, layerCovering(40), ChunkMesh::LayerInfo{}};

    const TerrainTileMapPixels pixels = terrainTileMapPixels(*mesh);

    REQUIRE(coverAt(pixels, TerrainChunkSlot::at(0, 0), 100)[1] == 255);
}

TEST_CASE("An alpha map short of 64x64 covers what it leaves out", "[terrain][maps]") {
    auto mesh = std::make_unique<TerrainMesh>();
    ChunkMesh& chunk = mesh->getChunk(7, 7);
    makeDrawn(chunk);
    chunk.layers = {ChunkMesh::LayerInfo{}, layerCovering(60, 2048)};

    const TerrainTileMapPixels pixels = terrainTileMapPixels(*mesh);

    REQUIRE(coverAt(pixels, TerrainChunkSlot::at(7, 7), 2047)[0] == 60);
    REQUIRE(coverAt(pixels, TerrainChunkSlot::at(7, 7), 3000)[0] == 255);
}

TEST_CASE("A chunk is lit from its own normals", "[terrain][maps]") {
    auto mesh = std::make_unique<TerrainMesh>();
    ChunkMesh& chunk = mesh->getChunk(15, 2);
    makeDrawn(chunk);
    chunk.normalGrid.assign(kNormalTexels * 4, 0);
    chunk.normalGrid[0] = 11;
    chunk.normalGrid[1] = 22;
    chunk.normalGrid[2] = 33;
    chunk.normalGrid[3] = 44;

    const TerrainTileMapPixels pixels = terrainTileMapPixels(*mesh);

    REQUIRE(normalAt(pixels, TerrainChunkSlot::at(15, 2), 0) == std::vector<int>{11, 22, 33, 44});
    REQUIRE(pixels.chunksLitFlat == 0);
}

TEST_CASE("A chunk without normals is lit as flat ground", "[terrain][maps]") {
    auto mesh = std::make_unique<TerrainMesh>();
    makeDrawn(mesh->getChunk(1, 9));

    const TerrainTileMapPixels pixels = terrainTileMapPixels(*mesh);

    REQUIRE(normalAt(pixels, TerrainChunkSlot::at(1, 9), 5) == std::vector<int>{128, 128, 255, 128});
    REQUIRE(pixels.chunksLitFlat == 1);
}
