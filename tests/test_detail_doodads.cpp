// Ground clutter laid out by the 3.3.5a client's rules (WoW.exe 12340,
// CMapChunk::CreateDetailDoodads): which quads of a chunk grow anything, what
// grows there, and how each doodad stands.
//
// The generator's expected draws below were produced by running the client's
// own code - its seed and next functions at 0x4c1510 and 0x464580, executed in
// an x86 emulator over the bytes of WoW.exe - not by this port.
//
// The chunks are built by hand, so this needs no ADT, no device and no world.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include <glm/glm.hpp>

#include "pipeline/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/client_random.hpp"
#include "pipeline/detail_doodads.hpp"

using namespace wowee::pipeline;

namespace {

constexpr float kChunk = wowee::core::coords::TILE_SIZE / 16.0f;
constexpr float kQuad = kChunk / 8.0f;
constexpr uint32_t kGrassEffect = 7;
constexpr uint32_t kTuft = 1;
constexpr uint32_t kFlower = 2;

/// A chunk at the given corner whose ground rises `riseX` yards per yard along
/// the grid's x, with one texture layer growing `effect` everywhere.
MapChunk chunkAt(float x0, float y0, float riseX = 0.0f, uint32_t effect = kGrassEffect) {
    MapChunk chunk;
    chunk.position[0] = x0;
    chunk.position[1] = y0;
    chunk.position[2] = 50.0f;
    // MCVT: 9 outer then 8 inner per row; grid x of an outer vertex is its
    // column, of an inner one its column plus a half.
    for (int row = 0, i = 0; row < 17; ++row) {
        const bool inner = (row % 2) == 1;
        const int count = inner ? 8 : 9;
        for (int c = 0; c < count; ++c, ++i) {
            const float gx = static_cast<float>(c) + (inner ? 0.5f : 0.0f);
            chunk.heightMap.heights[i] = riseX * gx * kQuad;
        }
    }
    chunk.heightMap.loaded = true;
    chunk.layers.push_back({0, 0, 0, effect});
    return chunk;
}

GroundEffectTables tables(std::array<uint32_t, 4> weights = {16, 0, 0, 0}, uint32_t density = 8) {
    GroundEffectTables t;
    GroundEffectTexture e;
    e.doodadIds = {kTuft, kFlower, 0, 0};
    e.weights = weights;
    e.density = density;
    t.textures[kGrassEffect] = e;
    t.doodads[kTuft] = {"World\\NoDXT\\Detail\\ElwGra01.m2", 0};
    t.doodads[kFlower] = {"World\\NoDXT\\Detail\\ElwFlo01.m2", 0};
    return t;
}

/// Which quad a placement stands in, from its position: world X runs down the
/// grid's y and world Y down its x.
glm::ivec2 quadOf(const MapChunk& chunk, const DetailDoodadPlacement& p) {
    const float gx = (chunk.position[1] - p.position.y) / kQuad;
    const float gy = (chunk.position[0] - p.position.x) / kQuad;
    return {static_cast<int>(std::floor(gx)), static_cast<int>(std::floor(gy))};
}

}  // namespace

TEST_CASE("The client's generator draws what the client's code draws", "[detail_doodads]") {
    struct Expected { uint32_t seed; uint32_t draws[6]; };
    const Expected cases[] = {
        {0x00000000u, {0x16be8aa9u, 0xba39c860u, 0xb1dfd560u, 0x6bbd62dfu, 0x900f3784u, 0xde0646ceu}},
        {0x00003039u, {0xa04d0406u, 0x3098dc7du, 0xa3886c37u, 0xecd344d5u, 0x9b8992eau, 0xbf407eb9u}},
        // Row 40, column 31: the seed of a real chunk.
        {0x0028001fu, {0xba013927u, 0xe5783362u, 0x3ccc733bu, 0x2519c831u, 0xd8d8cb54u, 0xb3885001u}},
        {0xdeadbeefu, {0x2a7e5c56u, 0x00a4e17du, 0x308e76f5u, 0xcb2fb73au, 0x45df7f1au, 0xa9054b9eu}},
    };
    for (const auto& c : cases) {
        INFO("seed " << c.seed);
        ClientRandom random(c.seed);
        for (uint32_t expected : c.draws) CHECK(random.next() == expected);
    }
    CHECK(detailDoodadSeed(31, 40) == 0x0028001fu);
}

TEST_CASE("A drawn quad plants its effect's density, and 8 when the table says 0", "[detail_doodads]") {
    const MapChunk chunk = chunkAt(1000.0f, 2000.0f);
    // Sixteen draws, every quad growing on flat ground: sixteen quads' worth.
    CHECK(placeDetailDoodads(chunk, 31, 40, tables({16, 0, 0, 0}, 3), 16).placements.size() == 48);
    CHECK(placeDetailDoodads(chunk, 31, 40, tables({16, 0, 0, 0}, 0), 16).placements.size() == 128);
    // More draws, more clutter.
    CHECK(placeDetailDoodads(chunk, 31, 40, tables(), 256).placements.size() == 256 * 8);
}

TEST_CASE("Clutter stands on the ground of its own chunk, turned and scaled as the client does",
          "[detail_doodads]") {
    const MapChunk chunk = chunkAt(1000.0f, 2000.0f);
    const auto layout = placeDetailDoodads(chunk, 31, 40, tables(), 64);
    REQUIRE_FALSE(layout.placements.empty());
    std::set<float> rotations;
    for (const auto& p : layout.placements) {
        CHECK(p.position.x <= chunk.position[0]);
        CHECK(p.position.x >= chunk.position[0] - kChunk);
        CHECK(p.position.y <= chunk.position[1]);
        CHECK(p.position.y >= chunk.position[1] - kChunk);
        CHECK(p.position.z == Catch::Approx(chunk.position[2]));
        CHECK(p.groundNormal.z == Catch::Approx(1.0f));
        CHECK(p.scale >= 0.67f - 1e-4f);
        CHECK(p.scale <= 1.33f + 1e-4f);
        CHECK(p.rotation >= 0.0f);
        CHECK(p.rotation <= 2.0f * 3.14159266f);
        rotations.insert(p.rotation);
    }
    CHECK(rotations.size() > layout.placements.size() / 2);
}

TEST_CASE("The same chunk is laid out the same every time, and its neighbour differently",
          "[detail_doodads]") {
    const MapChunk chunk = chunkAt(1000.0f, 2000.0f);
    const auto a = placeDetailDoodads(chunk, 31, 40, tables(), 32);
    const auto b = placeDetailDoodads(chunk, 31, 40, tables(), 32);
    const auto other = placeDetailDoodads(chunk, 32, 40, tables(), 32);
    REQUIRE(a.placements.size() == b.placements.size());
    for (size_t i = 0; i < a.placements.size(); ++i) {
        CHECK(a.placements[i].position == b.placements[i].position);
        CHECK(a.placements[i].rotation == b.placements[i].rotation);
    }
    CHECK(a.placements.front().position != other.placements.front().position);
}

TEST_CASE("Quads the map forbids clutter on, and holes, grow nothing", "[detail_doodads]") {
    MapChunk chunk = chunkAt(1000.0f, 2000.0f);
    // The left half of the grid forbidden.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) chunk.noEffectDoodad |= uint64_t{1} << (y * 8 + x);
    }
    // And the top-right quarter a hole (holes are 2x2 quads a bit).
    for (int hy = 0; hy < 2; ++hy) {
        for (int hx = 2; hx < 4; ++hx) chunk.holes |= static_cast<uint16_t>(1u << (hy * 4 + hx));
    }
    const auto layout = placeDetailDoodads(chunk, 31, 40, tables(), 256);
    REQUIRE_FALSE(layout.placements.empty());
    for (const auto& p : layout.placements) {
        const glm::ivec2 q = quadOf(chunk, p);
        CHECK(q.x >= 4);
        CHECK_FALSE((q.x >= 4 && q.y < 4));
    }
}

TEST_CASE("A quad grows what the layer its low-quality map names grows", "[detail_doodads]") {
    MapChunk chunk = chunkAt(1000.0f, 2000.0f);
    // Layer 1 is bare ground; the right half of every row maps to it.
    chunk.layers.push_back({1, 0x100, 0, 0});
    for (int y = 0; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            const int quad = y * 8 + x;
            chunk.doodadMapping[quad / 4] |= static_cast<uint8_t>(1u << ((quad % 4) * 2));
        }
    }
    const auto layout = placeDetailDoodads(chunk, 31, 40, tables(), 256);
    REQUIRE_FALSE(layout.placements.empty());
    for (const auto& p : layout.placements) CHECK(quadOf(chunk, p).x < 4);
}

TEST_CASE("Nothing grows on ground steeper than the client allows", "[detail_doodads]") {
    // A normal of 0.4 up is a slope of about 2.3 yards per yard.
    CHECK(placeDetailDoodads(chunkAt(0.0f, 0.0f, 3.0f), 31, 40, tables(), 64).placements.empty());
    const auto gentle = placeDetailDoodads(chunkAt(0.0f, 0.0f, 1.0f), 31, 40, tables(), 64);
    REQUIRE_FALSE(gentle.placements.empty());
    for (const auto& p : gentle.placements) {
        CHECK(p.groundNormal.z == Catch::Approx(1.0f / std::sqrt(2.0f)).margin(1e-4));
    }
}

TEST_CASE("Each doodad of an effect grows in proportion to its weight", "[detail_doodads]") {
    const MapChunk chunk = chunkAt(1000.0f, 2000.0f);
    auto count = [&](std::array<uint32_t, 4> weights) {
        size_t tufts = 0, flowers = 0;
        for (const auto& p : placeDetailDoodads(chunk, 31, 40, tables(weights), 256).placements) {
            (p.doodadId == kTuft ? tufts : flowers)++;
        }
        return std::pair{tufts, flowers};
    };
    const auto [onlyTufts, noFlowers] = count({16, 0, 0, 0});
    CHECK(onlyTufts > 0);
    CHECK(noFlowers == 0);
    const auto [tufts, flowers] = count({8, 8, 0, 0});
    CHECK(tufts > 0);
    CHECK(flowers > 0);
    CHECK(std::abs(static_cast<int>(tufts) - static_cast<int>(flowers)) <
          static_cast<int>(tufts + flowers) / 4);
}

TEST_CASE("Ids the tables do not have are reported, not planted", "[detail_doodads]") {
    const MapChunk unknownEffect = chunkAt(1000.0f, 2000.0f, 0.0f, 999);
    const auto a = placeDetailDoodads(unknownEffect, 31, 40, tables(), 16);
    CHECK(a.placements.empty());
    CHECK(a.unknownEffects == std::vector<uint32_t>{999});

    GroundEffectTables t = tables();
    t.doodads.erase(kTuft);
    const auto b = placeDetailDoodads(chunkAt(1000.0f, 2000.0f), 31, 40, t, 16);
    CHECK(b.placements.empty());
    CHECK(b.unknownDoodads == std::vector<uint32_t>{kTuft});
}

TEST_CASE("Flat clutter lies along the slope and the rest stands upright", "[detail_doodads]") {
    DetailDoodadPlacement p;
    p.groundNormal = glm::normalize(glm::vec3(0.5f, 0.0f, 1.0f));
    p.rotation = 1.0f;
    p.scale = 1.2f;
    const glm::vec3 lyingUp = detailDoodadBasis(p, true) * glm::vec3(0.0f, 0.0f, 1.0f);
    CHECK(glm::length(lyingUp) == Catch::Approx(1.2f));
    CHECK(glm::dot(glm::normalize(lyingUp), p.groundNormal) == Catch::Approx(1.0f));
    const glm::vec3 standingUp = detailDoodadBasis(p, false) * glm::vec3(0.0f, 0.0f, 1.0f);
    CHECK(standingUp.z == Catch::Approx(1.2f));
    // Turned about up by the rotation either way.
    const glm::vec3 x = detailDoodadBasis(p, false) * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(std::atan2(x.y, x.x) == Catch::Approx(1.0f));
}

TEST_CASE("A chunk's clutter is one mesh, a batch per texture, lit by the ground", "[detail_doodads]") {
    DetailDoodadModel tuft;
    tuft.vertices = {{{0, 0, 0}, {0, 0}}, {{1, 0, 0}, {1, 0}}, {{0, 0, 1}, {0, 1}}};
    tuft.indices = {0, 1, 2};
    tuft.batches = {{"grass.blp", 0, 3}};
    DetailDoodadModel flower = tuft;
    flower.batches = {{"flower.blp", 0, 3}};

    std::vector<DetailDoodadPlacement> placements(3);
    placements[0].doodadId = kTuft;
    placements[1].doodadId = kFlower;
    placements[2].doodadId = kTuft;
    placements[2].position = glm::vec3(10.0f, 0.0f, 0.0f);
    placements[2].groundNormal = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));

    const DetailChunkMesh mesh = buildDetailChunkMesh(placements, tables(), [&](uint32_t id) {
        return id == kTuft ? &tuft : &flower;
    });
    REQUIRE(mesh.batches.size() == 2);
    CHECK(mesh.vertices.size() == 9);
    for (const auto& b : mesh.batches) CHECK(b.indexCount == (b.texture == "grass.blp" ? 6u : 3u));
    CHECK(mesh.vertices[6].position.x == Catch::Approx(10.0f));
    CHECK(mesh.vertices[6].normal == placements[2].groundNormal);
    CHECK(mesh.boundsMax.x == Catch::Approx(11.0f));
}
