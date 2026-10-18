// The levels of detail a terrain chunk is drawn at from far away: each covers
// the same ground as the full mesh with fewer triangles, keeps the chunk's
// holes open or is not offered at all, and hangs skirts deep enough that two
// chunks at neighbouring levels never leave a gap between their edges.
#include <catch_amalgamated.hpp>

#include "pipeline/terrain_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using wowee::pipeline::ADTTerrain;
using wowee::pipeline::ChunkMesh;
using wowee::pipeline::kTerrainLodLevels;
using wowee::pipeline::TerrainIndex;
using wowee::pipeline::TerrainMeshGenerator;

namespace {

/// A tile whose chunk (0, 0) has the heights `rule` gives at grid (x, y),
/// inner vertices included, and the hole mask `holes`.
template <typename Rule>
std::unique_ptr<ADTTerrain> tileWith(Rule rule, uint16_t holes = 0) {
    auto tile = std::make_unique<ADTTerrain>();
    tile->loaded = true;
    tile->coord = {32, 32};
    auto& chunk = tile->chunks[0];
    chunk.holes = holes;
    chunk.normals.fill(0);
    chunk.heightMap.loaded = true;
    chunk.heightMap.heights.fill(0.0f);
    for (int y = 0; y <= 8; ++y)
        for (int x = 0; x <= 8; ++x)
            chunk.heightMap.heights[static_cast<size_t>(y * 17 + x)] =
                rule(static_cast<float>(x), static_cast<float>(y));
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            chunk.heightMap.heights[static_cast<size_t>(9 + y * 17 + x)] =
                rule(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
    return tile;
}

/// A hill with a ridge across it: no two levels draw the same edges.
float hilly(float x, float y) {
    return 12.0f * std::sin(x * 0.9f) * std::cos(y * 0.7f) + 3.0f * std::sin(x * 2.7f + y * 1.9f);
}

/// Level 0, or one of the coarser ones; empty when it is not offered.
std::vector<TerrainIndex> levelIndices(const ChunkMesh& mesh, int lod) {
    if (lod == 0) return mesh.indices;
    const auto& range = mesh.lods[static_cast<size_t>(lod - 1)];
    return {mesh.lodIndices.begin() + range.first, mesh.lodIndices.begin() + range.first + range.count};
}

bool isSkirt(const ChunkMesh& mesh, const TerrainIndex* tri) {
    return tri[0] >= mesh.skirtFirstVertex || tri[1] >= mesh.skirtFirstVertex ||
           tri[2] >= mesh.skirtFirstVertex;
}

glm::vec3 position(const ChunkMesh& mesh, TerrainIndex i) {
    const auto& p = mesh.vertices[i].position;
    return {p[0], p[1], p[2]};
}

/// The surface triangles of a level, skirts left out.
std::vector<std::array<TerrainIndex, 3>> surface(const ChunkMesh& mesh, int lod) {
    const auto indices = levelIndices(mesh, lod);
    std::vector<std::array<TerrainIndex, 3>> out;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (!isSkirt(mesh, &indices[i])) out.push_back({indices[i], indices[i + 1], indices[i + 2]});
    }
    return out;
}

float footprintArea(const ChunkMesh& mesh, const std::array<TerrainIndex, 3>& t) {
    const glm::vec3 a = position(mesh, t[0]), b = position(mesh, t[1]), c = position(mesh, t[2]);
    return 0.5f * std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

bool footprintContains(const ChunkMesh& mesh, const std::array<TerrainIndex, 3>& t, glm::vec2 p) {
    const glm::vec3 a = position(mesh, t[0]), b = position(mesh, t[1]), c = position(mesh, t[2]);
    const auto side = [](glm::vec2 u, glm::vec2 v, glm::vec2 w) {
        return (v.x - u.x) * (w.y - u.y) - (w.x - u.x) * (v.y - u.y);
    };
    const float d1 = side({a.x, a.y}, {b.x, b.y}, p);
    const float d2 = side({b.x, b.y}, {c.x, c.y}, p);
    const float d3 = side({c.x, c.y}, {a.x, a.y}, p);
    return (d1 > 0 && d2 > 0 && d3 > 0) || (d1 < 0 && d2 < 0 && d3 < 0);
}

/// Outer vertex i (0-8) along an edge: row 0, row 8, column 0, column 8.
int edgeVertex(int edge, int i) {
    switch (edge) {
        case 0: return i;
        case 1: return 8 * 17 + i;
        case 2: return i * 17;
        default: return i * 17 + 8;
    }
}

}  // namespace

TEST_CASE("each coarser level covers the same ground with fewer triangles", "[terrainlod]") {
    const auto tile = tileWith(hilly);
    const ChunkMesh mesh = TerrainMeshGenerator::generate(*tile).getChunk(0, 0);
    REQUIRE(mesh.isValid());

    float fullArea = 0.0f;
    for (const auto& t : surface(mesh, 0)) fullArea += footprintArea(mesh, t);
    REQUIRE(fullArea > 0.0f);

    size_t previous = surface(mesh, 0).size();
    for (int lod = 1; lod < kTerrainLodLevels; ++lod) {
        INFO("level " << lod);
        const auto tris = surface(mesh, lod);
        REQUIRE_FALSE(tris.empty());
        float area = 0.0f;
        for (const auto& t : tris) area += footprintArea(mesh, t);
        CHECK(area == Catch::Approx(fullArea).epsilon(1e-4));
        CHECK(tris.size() < previous);
        previous = tris.size();
    }
}

TEST_CASE("a level keeps the chunk's holes open, or is not offered", "[terrainlod]") {
    // Bit 0: the four quads in the chunk's first two rows and columns.
    const auto tile = tileWith(hilly, 0x0001);
    const ChunkMesh mesh = TerrainMeshGenerator::generate(*tile).getChunk(0, 0);
    REQUIRE(mesh.isValid());

    // The middle of the first quad, which is the hole.
    const glm::vec3 holeMiddle = position(mesh, 9);
    for (int lod = 0; lod < kTerrainLodLevels; ++lod) {
        INFO("level " << lod);
        for (const auto& t : surface(mesh, lod)) {
            CHECK_FALSE(footprintContains(mesh, t, {holeMiddle.x, holeMiddle.y}));
        }
    }
    // Levels 1 and 2 have cells no bigger than the hole's; level 3's would
    // have to cover a quarter hole, so it is not offered.
    CHECK(mesh.lods[0].count > 0);
    CHECK(mesh.lods[1].count > 0);
    CHECK(mesh.lods[2].count == 0);
}

TEST_CASE("the skirts reach below every level's edge, so no seam opens", "[terrainlod]") {
    const auto tile = tileWith(hilly);
    const ChunkMesh mesh = TerrainMeshGenerator::generate(*tile).getChunk(0, 0);
    REQUIRE(mesh.isValid());

    // A level's edge height at outer position i of an edge: along its own
    // stride, between the vertices it keeps.
    const auto edgeHeight = [&](int lod, int edge, int i) {
        const int stride = lod == 0 ? 1 : 1 << (lod - 1);
        const int from = std::min(i / stride * stride, 8 - stride);
        const float t = static_cast<float>(i - from) / static_cast<float>(stride);
        return position(mesh, edgeVertex(edge, from)).z * (1.0f - t) +
               position(mesh, edgeVertex(edge, from + stride)).z * t;
    };
    const auto skirtBottom = [&](int lod, int edge, int i) {
        const int stride = 1 << (lod - 1);
        const int from = std::min(i / stride * stride, 8 - stride);
        const float t = static_cast<float>(i - from) / static_cast<float>(stride);
        const auto low = [&](int j) {
            return position(mesh, static_cast<TerrainIndex>(mesh.skirtFirstVertex + edge * 9 + j)).z;
        };
        return low(from) * (1.0f - t) + low(from + stride) * t;
    };

    for (int lod = 1; lod < kTerrainLodLevels; ++lod) {
        // Every segment of every edge has its strip.
        const auto indices = levelIndices(mesh, lod);
        size_t skirtTriangles = 0;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) skirtTriangles += isSkirt(mesh, &indices[i]) ? 1 : 0;
        CHECK(skirtTriangles == static_cast<size_t>(4 * (8 >> (lod - 1)) * 2));

        for (int edge = 0; edge < 4; ++edge) {
            for (int i = 0; i <= 8; ++i) {
                float lowestEdge = edgeHeight(0, edge, i);
                for (int other = 1; other < kTerrainLodLevels; ++other)
                    lowestEdge = std::min(lowestEdge, edgeHeight(other, edge, i));
                INFO("level " << lod << " edge " << edge << " at " << i);
                CHECK(skirtBottom(lod, edge, i) < lowestEdge);
            }
        }
    }
}

TEST_CASE("no skirt hangs beside a hole", "[terrainlod]") {
    // Bit 0 again: the quads at the start of row 0 and of column 0.
    const auto tile = tileWith(hilly, 0x0001);
    const ChunkMesh mesh = TerrainMeshGenerator::generate(*tile).getChunk(0, 0);
    REQUIRE(mesh.isValid());

    const auto indices = levelIndices(mesh, 1);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (!isSkirt(mesh, &indices[i])) continue;
        for (int k = 0; k < 3; ++k) {
            const TerrainIndex v = indices[i + k];
            // Row 0 positions 0-2 and column 0 positions 0-2 border the hole;
            // the strips beside it would start at 0 or 1 and end at 1 or 2.
            const bool beside = v == 0 || v == 1 || v == 17 ||
                                v == mesh.skirtFirstVertex + 0 || v == mesh.skirtFirstVertex + 1 ||
                                v == mesh.skirtFirstVertex + 2 * 9 + 0 || v == mesh.skirtFirstVertex + 2 * 9 + 1;
            CHECK_FALSE(beside);
        }
    }
}
