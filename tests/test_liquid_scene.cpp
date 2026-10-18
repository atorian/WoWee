// The map's liquid as the client knows it: where the water is and how high,
// which body each stretch of it belongs to, and what the water renderer is
// told as tiles stream in and out.
//
// Everything is driven the way the terrain streaming drives it - a tile's
// MH2O layers or a building's MLIQ in, removeTile / removeWmo / clear - and
// read back the way the camera, swimming and the renderer read it.
#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/liquid_scene.hpp"

#include <algorithm>
#include <vector>

using wowee::pipeline::ADTTerrain;
using wowee::pipeline::WMOLiquid;
using wowee::rendering::LiquidBody;
using wowee::rendering::LiquidScene;
using wowee::rendering::LiquidSceneListener;
using wowee::rendering::LiquidSurface;
using wowee::rendering::LiquidSurfaceId;

namespace {

constexpr float kTile = 533.33333f;
constexpr float kChunk = kTile / 16.0f;
constexpr float kCell = kChunk / 8.0f;
constexpr float kChunkArea = 64.0f * kCell * kCell;

// MH2O liquid types: 1 inland water, 2 ocean.
constexpr uint16_t kWater = 1;
constexpr uint16_t kOcean = 2;

/// A tile laid out as the ADT loader leaves one: chunk (x, y)'s north-west
/// corner, rows running down render X and columns down render Y, and tile
/// (tx, ty + 1) continuing where tile (tx, ty)'s last row ends.
ADTTerrain tile(int tileX, int tileY) {
    ADTTerrain t;
    const float baseX = (32.0f - static_cast<float>(tileY)) * kTile;
    const float baseY = (32.0f - static_cast<float>(tileX)) * kTile;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            auto& c = t.getChunk(cx, cy);
            c.position[0] = baseX - static_cast<float>(cy) * kChunk;
            c.position[1] = baseY - static_cast<float>(cx) * kChunk;
            c.position[2] = 0.0f;
        }
    }
    return t;
}

/// A whole chunk of flat liquid, in the chunk-wide 8x8 mask the loader
/// normalises MH2O and MCLQ into.
void flood(ADTTerrain& t, int cx, int cy, uint16_t type, float height) {
    ADTTerrain::WaterLayer layer{};
    layer.liquidType = type;
    layer.minHeight = height;
    layer.maxHeight = height;
    layer.x = 0;
    layer.y = 0;
    layer.width = 8;
    layer.height = 8;
    layer.mask.assign(8, 0xFF);
    t.waterData[static_cast<size_t>(cy) * 16 + cx].layers.push_back(layer);
}

/// The middle of a chunk of a tile, in render coordinates.
glm::vec2 middleOf(int tileX, int tileY, int cx, int cy) {
    const float baseX = (32.0f - static_cast<float>(tileY)) * kTile;
    const float baseY = (32.0f - static_cast<float>(tileX)) * kTile;
    return {baseX - (static_cast<float>(cy) + 0.5f) * kChunk,
            baseY - (static_cast<float>(cx) + 0.5f) * kChunk};
}

struct Recorder : LiquidSceneListener {
    std::vector<LiquidSurfaceId> added;
    std::vector<LiquidSurfaceId> removed;
    void liquidSurfaceAdded(const LiquidSurface& s) override { added.push_back(s.id); }
    void liquidSurfaceRemoved(LiquidSurfaceId id) override { removed.push_back(id); }
};

}  // namespace

TEST_CASE("water stands where a tile's layers put it and nowhere else", "[liquid]") {
    LiquidScene scene;
    ADTTerrain t = tile(32, 48);
    flood(t, 3, 4, kWater, 12.5f);
    scene.addTerrainTile(t, 32, 48);

    const glm::vec2 wet = middleOf(32, 48, 3, 4);
    const glm::vec2 dry = middleOf(32, 48, 9, 4);
    REQUIRE(scene.heightAt(wet.x, wet.y));
    CHECK(*scene.heightAt(wet.x, wet.y) == Catch::Approx(12.5f));
    CHECK(scene.typeAt(wet.x, wet.y) == kWater);
    CHECK_FALSE(scene.heightAt(dry.x, dry.y));
    CHECK_FALSE(scene.bodyAt(dry.x, dry.y));
}

TEST_CASE("a pond across neighbouring chunks is one body of their area", "[liquid]") {
    LiquidScene scene;
    ADTTerrain t = tile(32, 48);
    flood(t, 2, 2, kWater, 10.0f);
    flood(t, 3, 2, kWater, 10.0f);
    flood(t, 2, 3, kWater, 10.0f);
    scene.addTerrainTile(t, 32, 48);

    const auto bodies = scene.bodies();
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].area == Catch::Approx(3.0f * kChunkArea));
    CHECK(bodies[0].basicType == 0);
    CHECK_FALSE(bodies[0].inWmo);

    const glm::vec2 a = middleOf(32, 48, 2, 2);
    const glm::vec2 b = middleOf(32, 48, 2, 3);
    REQUIRE(scene.bodyAt(a.x, a.y));
    REQUIRE(scene.bodyAt(b.x, b.y));
    CHECK(scene.bodyAt(a.x, a.y)->id == scene.bodyAt(b.x, b.y)->id);
}

TEST_CASE("ponds that do not touch are separate bodies", "[liquid]") {
    LiquidScene scene;
    ADTTerrain t = tile(32, 48);
    flood(t, 1, 1, kWater, 10.0f);
    flood(t, 6, 6, kWater, 10.0f);
    scene.addTerrainTile(t, 32, 48);

    const auto bodies = scene.bodies();
    REQUIRE(bodies.size() == 2);
    for (const auto& b : bodies) CHECK(b.area == Catch::Approx(kChunkArea));
}

TEST_CASE("lakes inside one merged tile surface are told apart", "[liquid]") {
    // Six chunks of water at one height are drawn as one tile-wide surface;
    // the two clusters of them are still two lakes.
    LiquidScene scene;
    ADTTerrain t = tile(32, 48);
    flood(t, 0, 0, kWater, 10.0f);
    flood(t, 1, 0, kWater, 10.0f);
    flood(t, 0, 1, kWater, 10.0f);
    flood(t, 10, 10, kWater, 10.0f);
    flood(t, 11, 10, kWater, 10.0f);
    flood(t, 10, 11, kWater, 10.0f);
    scene.addTerrainTile(t, 32, 48);

    REQUIRE(scene.surfaceCount() == 1);
    const auto bodies = scene.bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0].area == Catch::Approx(3.0f * kChunkArea));
    CHECK(bodies[1].area == Catch::Approx(3.0f * kChunkArea));
}

TEST_CASE("a lake across a tile edge is one body until a tile unloads", "[liquid]") {
    LiquidScene scene;
    ADTTerrain north = tile(32, 48);
    ADTTerrain south = tile(32, 49);
    flood(north, 4, 15, kWater, 10.0f);  // the north tile's last row
    flood(south, 4, 0, kWater, 10.0f);   // the south tile's first
    scene.addTerrainTile(north, 32, 48);
    scene.addTerrainTile(south, 32, 49);

    auto bodies = scene.bodies();
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].area == Catch::Approx(2.0f * kChunkArea));

    scene.removeTile(32, 49);
    bodies = scene.bodies();
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].area == Catch::Approx(kChunkArea));
    const glm::vec2 gone = middleOf(32, 49, 4, 0);
    CHECK_FALSE(scene.heightAt(gone.x, gone.y));
}

TEST_CASE("a body keeps its id while its oldest water stays loaded", "[liquid]") {
    LiquidScene scene;
    ADTTerrain north = tile(32, 48);
    ADTTerrain south = tile(32, 49);
    flood(north, 4, 15, kWater, 10.0f);
    flood(south, 4, 0, kWater, 10.0f);
    scene.addTerrainTile(north, 32, 48);
    const glm::vec2 p = middleOf(32, 48, 4, 15);
    const auto before = scene.bodyAt(p.x, p.y);
    REQUIRE(before);

    scene.addTerrainTile(south, 32, 49);
    const auto joined = scene.bodyAt(p.x, p.y);
    REQUIRE(joined);
    CHECK(joined->id == before->id);
    CHECK(joined->area > before->area);
}

TEST_CASE("a river's gentle slope stays one body, a waterfall splits it", "[liquid]") {
    SECTION("a step of a yard") {
        LiquidScene scene;
        ADTTerrain t = tile(32, 48);
        flood(t, 5, 5, kWater, 10.0f);
        flood(t, 5, 6, kWater, 11.0f);
        scene.addTerrainTile(t, 32, 48);
        CHECK(scene.bodies().size() == 1);
    }
    SECTION("a drop of six yards") {
        LiquidScene scene;
        ADTTerrain t = tile(32, 48);
        flood(t, 5, 5, kWater, 10.0f);
        flood(t, 5, 6, kWater, 4.0f);
        scene.addTerrainTile(t, 32, 48);
        CHECK(scene.bodies().size() == 2);
    }
}

TEST_CASE("a lake over a flooded cave is two bodies, and the lake is the one above", "[liquid]") {
    LiquidScene scene;
    ADTTerrain t = tile(32, 48);
    flood(t, 7, 7, kWater, 20.0f);
    flood(t, 7, 7, kWater, 2.0f);
    scene.addTerrainTile(t, 32, 48);

    REQUIRE(scene.bodies().size() == 2);
    const glm::vec2 p = middleOf(32, 48, 7, 7);
    REQUIRE(scene.heightAt(p.x, p.y));
    CHECK(*scene.heightAt(p.x, p.y) == Catch::Approx(20.0f));
    REQUIRE(scene.bodyAt(p.x, p.y));
    CHECK(scene.bodyAt(p.x, p.y)->maxHeight == Catch::Approx(20.0f));
    // The camera down in the cave finds the cave's water, not the lake's.
    REQUIRE(scene.nearestHeightAt(p.x, p.y, 1.0f, 15.0f));
    CHECK(*scene.nearestHeightAt(p.x, p.y, 1.0f, 15.0f) == Catch::Approx(2.0f));
}

TEST_CASE("the sea and a lake that meet it are different bodies", "[liquid]") {
    LiquidScene scene;
    ADTTerrain t = tile(32, 48);
    flood(t, 8, 8, kOcean, 0.0f);
    flood(t, 8, 9, kWater, 0.5f);
    scene.addTerrainTile(t, 32, 48);

    const auto bodies = scene.bodies();
    REQUIRE(bodies.size() == 2);
    const glm::vec2 sea = middleOf(32, 48, 8, 8);
    REQUIRE(scene.bodyAt(sea.x, sea.y));
    CHECK(scene.bodyAt(sea.x, sea.y)->basicType == 1);
}

TEST_CASE("a building's pool is liquid in the building", "[liquid]") {
    LiquidScene scene;
    WMOLiquid pool;
    pool.xVerts = 5;
    pool.yVerts = 5;
    pool.xTiles = 4;
    pool.yTiles = 4;
    pool.basePosition = glm::vec3(100.0f, 200.0f, 50.0f);
    pool.liquidType = kWater;
    REQUIRE(scene.addWmoLiquid(pool, glm::mat4(1.0f), 77, wowee::rendering::LiquidShelter::Open));

    const float x = 100.0f + 2.0f * 4.1666625f;
    const float y = 200.0f + 2.0f * 4.1666625f;
    CHECK(scene.isWmoLiquidAt(x, y));
    // At MLIQ's own height: a yard taken off it put shallow pools under their floors.
    REQUIRE(scene.heightAt(x, y));
    CHECK(*scene.heightAt(x, y) == Catch::Approx(50.0f));
    CHECK(scene.typeAt(x, y) == kWater);
    REQUIRE(scene.bodyAt(x, y));
    CHECK(scene.bodyAt(x, y)->inWmo);

    scene.removeWmo(77);
    CHECK_FALSE(scene.isWmoLiquidAt(x, y));
    CHECK(scene.empty());
}

TEST_CASE("liquid whose grid does not add up is not placed", "[liquid]") {
    LiquidScene scene;
    WMOLiquid broken;
    broken.xVerts = 5;
    broken.yVerts = 5;
    broken.xTiles = 7;  // five vertices make four tiles
    broken.yTiles = 4;
    broken.basePosition = glm::vec3(0.0f);
    CHECK_FALSE(scene.addWmoLiquid(broken, glm::mat4(1.0f), 9, wowee::rendering::LiquidShelter::Open));
    CHECK(scene.empty());
}

TEST_CASE("the water renderer is told of every surface as it comes and goes", "[liquid]") {
    LiquidScene scene;
    ADTTerrain a = tile(32, 48);
    flood(a, 1, 1, kWater, 10.0f);
    flood(a, 6, 6, kWater, 3.0f);
    scene.addTerrainTile(a, 32, 48);

    Recorder late;
    {
        auto sub = scene.subscribe(late);
        // A listener that arrives late is told of what is already there.
        CHECK(late.added.size() == scene.surfaceCount());

        ADTTerrain b = tile(32, 49);
        flood(b, 2, 2, kWater, 10.0f);
        scene.addTerrainTile(b, 32, 49);
        CHECK(late.added.size() == 3);

        scene.removeTile(32, 48);
        CHECK(late.removed.size() == 2);
        scene.clear();
        CHECK(late.removed.size() == 3);
        std::vector<LiquidSurfaceId> a1 = late.added, r1 = late.removed;
        std::sort(a1.begin(), a1.end());
        std::sort(r1.begin(), r1.end());
        CHECK(a1 == r1);
    }
    // Once the subscription ends, nothing more is said.
    ADTTerrain c = tile(33, 48);
    flood(c, 0, 0, kWater, 10.0f);
    scene.addTerrainTile(c, 33, 48);
    CHECK(late.added.size() == 3);
}
