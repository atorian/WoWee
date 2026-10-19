// The solid world of a map (collision/collision_world.hpp): what is a floor,
// what stops a mover, what a line of sight meets and what holds a point -
// asked of buildings, doodads and terrain placed as the renderers place them,
// and asked again after each moves, hides or leaves.
//
// No renderer and no device: the shapes are built here from a handful of
// triangles, so each answer can be worked out by hand.
#include <catch_amalgamated.hpp>

#include "collision/collision_world.hpp"
#include "core/coordinates.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>
#include <vector>

using wowee::collision::CollisionWorld;
using wowee::collision::M2Shape;
using wowee::collision::Reach;
using wowee::collision::TerrainTileHeights;
using wowee::collision::WmoGroupShape;
using wowee::collision::WmoShape;

namespace {

const Reach kEverywhere = Reach::everywhere();

glm::mat4 at(const glm::vec3& position) { return glm::translate(glm::mat4(1.0f), position); }

/// A flat square deck four yards across at z = 0, model space.
std::shared_ptr<const M2Shape> deck() {
    return M2Shape::build("deck", {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}},
                          {0, 1, 2, 0, 2, 3});
}

/// A wall three yards high standing on the plane x = 0.
std::shared_ptr<const M2Shape> post() {
    return M2Shape::build("post", {{0, -2, 0}, {0, 2, 0}, {0, 2, 3}, {0, -2, 3}},
                          {0, 1, 2, 0, 2, 3});
}

/// One building group from quads (four corners each), with its box.
struct Quad { glm::vec3 a, b, c, d; };
std::shared_ptr<const WmoGroupShape> group(const std::vector<Quad>& quads, uint32_t flags = 0,
                                           const glm::vec3& color = glm::vec3(-1.0f)) {
    std::vector<glm::vec3> vertices;
    std::vector<uint16_t> indices;
    for (const Quad& q : quads) {
        const auto base = static_cast<uint16_t>(vertices.size());
        vertices.insert(vertices.end(), {q.a, q.b, q.c, q.d});
        indices.insert(indices.end(), {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                                       base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)});
    }
    glm::vec3 lo = vertices.front(), hi = vertices.front();
    for (const auto& v : vertices) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    std::vector<glm::vec3> colors;
    if (color.x >= 0.0f) colors.assign(vertices.size(), color);
    return WmoGroupShape::build(flags, lo, hi, vertices, indices, colors, {});
}

Quad floorAt(float z, float half = 5.0f) {
    return {{-half, -half, z}, {half, -half, z}, {half, half, z}, {-half, half, z}};
}

std::shared_ptr<const WmoShape> building(std::vector<std::shared_ptr<const WmoGroupShape>> groups,
                                         uint32_t modelId = 7) {
    auto shape = std::make_shared<WmoShape>();
    shape->modelId = modelId;
    shape->groups = std::move(groups);
    shape->boundingBoxMin = shape->groups.front()->boundingBoxMin;
    shape->boundingBoxMax = shape->groups.front()->boundingBoxMax;
    for (const auto& g : shape->groups) {
        shape->boundingBoxMin = glm::min(shape->boundingBoxMin, g->boundingBoxMin);
        shape->boundingBoxMax = glm::max(shape->boundingBoxMax, g->boundingBoxMax);
    }
    shape->ambientColor = glm::vec3(0.3f, 0.2f, 0.1f);
    return shape;
}

constexpr uint32_t kIndoor = 0x2000;

}  // namespace

TEST_CASE("a doodad with a collision mesh is a floor where it stands, and not once it leaves",
          "[collision]") {
    CollisionWorld world;
    world.placeM2(1, deck(), at({10, 10, 5}), 1.0f);

    REQUIRE(world.m2Floor(kEverywhere, 10.5f, 9.5f, 6.0f) == Catch::Approx(5.0f));
    // Not above the probe plus its reach, and not where the deck is not.
    CHECK_FALSE(world.m2Floor(kEverywhere, 20.0f, 20.0f, 6.0f));

    world.removeM2(1);
    CHECK_FALSE(world.m2Floor(kEverywhere, 10.5f, 9.5f, 6.0f));
}

TEST_CASE("a doodad that moves is stood on where it went, not where it was", "[collision]") {
    // The grid it is found through is refiled on every move; one that kept the
    // old cells would answer from both places, or from neither.
    CollisionWorld world;
    world.placeM2(1, deck(), at({10, 10, 5}), 1.0f);
    world.moveM2(1, at({300, -200, 40}), 1.0f);

    CHECK_FALSE(world.m2Floor(kEverywhere, 10.0f, 10.0f, 6.0f));
    CHECK(world.m2Floor(kEverywhere, 300.0f, -200.0f, 41.0f) == Catch::Approx(40.0f));
}

TEST_CASE("a hidden doodad is nobody's floor until it is shown again", "[collision]") {
    // A ship's deck doodads, with the ship on the other continent.
    CollisionWorld world;
    world.placeM2(1, deck(), at({10, 10, 5}), 1.0f);
    world.setM2Hidden(1, true);

    CHECK_FALSE(world.m2Floor(kEverywhere, 10.0f, 10.0f, 6.0f));
    CHECK_FALSE(world.m2InstanceFloor(1, at({10, 10, 5}), 10.0f, 10.0f, 6.0f));

    world.setM2Hidden(1, false);
    CHECK(world.m2Floor(kEverywhere, 10.0f, 10.0f, 6.0f) == Catch::Approx(5.0f));
}

TEST_CASE("a doodad told to skip collision is no floor, one told to skip walls still is",
          "[collision]") {
    CollisionWorld world;
    world.placeM2(1, deck(), at({0, 0, 0}), 1.0f);
    world.placeM2(2, post(), at({0, 0, 0}), 1.0f);

    SECTION("skipping collision") {
        world.setM2SkipCollision(1, true);
        CHECK_FALSE(world.m2Floor(kEverywhere, 1.0f, 1.0f, 1.0f));
    }
    SECTION("skipping walls keeps the floor and lets a mover through the wall") {
        world.setM2SkipWalls(2, true);
        CHECK(world.m2Floor(kEverywhere, 1.0f, 1.0f, 1.0f) == Catch::Approx(0.0f));
        glm::vec3 adjusted;
        CHECK_FALSE(world.m2WallSlide(kEverywhere, {-1.0f, 0, 0.1f}, {-0.2f, 0, 0.1f}, adjusted));
    }
}

TEST_CASE("a doodad's wall pushes a mover gently back the way it came", "[collision]") {
    CollisionWorld world;
    world.placeM2(2, post(), at({0, 0, 0}), 1.0f);

    glm::vec3 adjusted;
    REQUIRE(world.m2WallSlide(kEverywhere, {-1.0f, 0, 0.1f}, {-0.2f, 0, 0.1f}, adjusted));
    CHECK(adjusted.x < -0.2f);
    CHECK(adjusted.y == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("a transport car's floor and box follow the pose it is asked at", "[collision]") {
    // The game asks where the car is this frame, which may be ahead of the
    // pose the collision world was last given.
    CollisionWorld world;
    world.placeM2(1, deck(), at({0, 0, 0}), 1.0f);

    glm::vec3 lo, hi;
    REQUIRE(world.m2InstanceBounds(1, at({100, 0, 20}), lo, hi));
    CHECK(lo.x == Catch::Approx(98.0f));
    CHECK(hi.x == Catch::Approx(102.0f));
    CHECK(lo.z == Catch::Approx(20.0f));
    CHECK(world.m2InstanceFloor(1, at({100, 0, 20}), 100.0f, 0.0f, 21.0f) == Catch::Approx(20.0f));
    CHECK_FALSE(world.m2InstanceFloor(1, at({100, 0, 20}), 0.0f, 0.0f, 1.0f));

    SECTION("and a car with no collision mesh is not one the world has") {
        world.placeM2(9, nullptr, at({0, 0, 0}), 1.0f);
        CHECK_FALSE(world.m2InstanceBounds(9, at({0, 0, 0}), lo, hi));
    }
}

TEST_CASE("a building's floor is the highest under the probe, or the nearest the feet",
          "[collision]") {
    // Two storeys: the ground floor at 10 and an upper one at 13.
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f), floorAt(3.0f)})}), at({50, 50, 10}));

    CHECK(world.wmoFloor(kEverywhere, 50.0f, 50.0f, 13.5f) == Catch::Approx(13.0f));
    // A player standing downstairs under the upper floor is not snapped up to
    // it: measured from the feet, the ground floor is nearer.
    CHECK(world.wmoFloor(kEverywhere, 50.0f, 50.0f, 13.5f, nullptr, 10.2f) == Catch::Approx(10.0f));
    CHECK(world.wmoFloor(kEverywhere, 50.0f, 50.0f, 11.0f) == Catch::Approx(10.0f));

    float normalZ = 0.0f;
    bool facesUp = false;
    world.wmoFloor(kEverywhere, 50.0f, 50.0f, 11.0f, &normalZ, std::nanf(""), &facesUp);
    CHECK(normalZ == Catch::Approx(1.0f));
}

TEST_CASE("a moving transport's deck is a floor only when it is underfoot", "[collision]") {
    // The Undercity lift: as it passes a bystander's height its deck must not
    // become their floor.
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f)})}), at({0, 0, 10}));
    world.setWmoTransport(1, true);

    CHECK(world.wmoFloor(kEverywhere, 0.0f, 0.0f, 12.0f, nullptr, 10.5f) == Catch::Approx(10.0f));
    CHECK_FALSE(world.wmoFloor(kEverywhere, 0.0f, 0.0f, 14.0f, nullptr, 14.0f));

    SECTION("an ordinary building's floor is taken from the same height") {
        world.setWmoTransport(1, false);
        CHECK(world.wmoFloor(kEverywhere, 0.0f, 0.0f, 14.0f, nullptr, 14.0f) == Catch::Approx(10.0f));
    }
}

TEST_CASE("a ship's deck is found where the game says the ship is this frame", "[collision]") {
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f)})}), at({0, 0, 10}));
    REQUIRE(world.hasWmo(1));

    CHECK(world.wmoInstanceFloor(1, at({200, 0, 30}), 200.0f, 0.0f, 31.0f) == Catch::Approx(30.0f));
    CHECK_FALSE(world.wmoInstanceFloor(1, at({200, 0, 30}), 0.0f, 0.0f, 11.0f));

    world.removeWmo(1);
    CHECK_FALSE(world.hasWmo(1));
}

TEST_CASE("a hidden building is no floor and holds no one", "[collision]") {
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f)}, kIndoor)}), at({0, 0, 10}));
    const uint64_t before = world.wmoRevision();
    world.setWmoHidden(1, true);

    CHECK(world.wmoRevision() != before);
    CHECK_FALSE(world.wmoFloor(kEverywhere, 0.0f, 0.0f, 11.0f));
    CHECK_FALSE(world.insideWmo(kEverywhere, 0.0f, 0.0f, 10.0f));
}

TEST_CASE("a building wall stops a character walking into it", "[collision]") {
    // A wall on x = 5, three yards high.
    CollisionWorld world;
    world.placeWmo(1, building({group({{{5, -5, 0}, {5, 5, 0}, {5, 5, 3}, {5, -5, 3}}})}), at({0, 0, 0}));

    glm::vec3 adjusted;
    REQUIRE(world.wmoWallSlide(kEverywhere, {4.0f, 0, 0}, {4.8f, 0, 0}, adjusted, false));
    // Held a radius off the wall.
    CHECK(adjusted.x == Catch::Approx(4.495f).margin(0.01f));

    SECTION("and a mover swept through it in steps ends on this side") {
        const glm::vec3 end = world.sweepMover(kEverywhere, {3.0f, 0, 0}, {7.0f, 0, 0}, false);
        CHECK(end.x < 5.0f);
    }
    SECTION("a wall far out of reach is not asked about") {
        CHECK_FALSE(world.wmoWallSlide(Reach::around({500, 500, 0}, 20.0f), {4.0f, 0, 0}, {4.8f, 0, 0},
                                       adjusted, false));
    }
}

TEST_CASE("a building wall blocks a line of sight through it and not one short of it",
          "[collision]") {
    CollisionWorld world;
    world.placeWmo(1, building({group({{{5, -5, 0}, {5, 5, 0}, {5, 5, 3}, {5, -5, 3}}})}), at({0, 0, 0}));

    CHECK(world.wmoSegmentBlocked(kEverywhere, {0, 0, 1}, {10, 0, 1}));
    CHECK_FALSE(world.wmoSegmentBlocked(kEverywhere, {0, 0, 1}, {4, 0, 1}));
    CHECK_FALSE(world.wmoSegmentBlocked(kEverywhere, {0, -8, 1}, {10, -8, 1}));
    CHECK(world.wmoRaycast(kEverywhere, {0, 0, 1}, {1, 0, 0}, 20.0f) == Catch::Approx(5.0f));
}

TEST_CASE("inside a building's group is inside it; only an interior group is indoors",
          "[collision]") {
    // Stormwind's streets are exterior groups of the city's WMO.
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f), floorAt(4.0f)})}, 42), at({0, 0, 0}));
    world.placeWmo(2, building({group({floorAt(0.0f), floorAt(4.0f)}, kIndoor)}, 43), at({100, 0, 0}));

    uint32_t modelId = 0;
    CHECK(world.insideWmo(kEverywhere, 0, 0, 1, &modelId));
    CHECK(modelId == 42);
    CHECK_FALSE(world.insideInteriorWmo(kEverywhere, 0, 0, 1));
    CHECK(world.insideInteriorWmo(kEverywhere, 100, 0, 1));
    CHECK_FALSE(world.insideWmo(kEverywhere, 50, 0, 1));
}

TEST_CASE("a unit in an interior group is lit by the building and the floor under it",
          "[collision]") {
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f), floorAt(4.0f)}, kIndoor, glm::vec3(0.6f, 0.4f, 0.2f))}),
                   at({0, 0, 0}));

    const auto inside = world.interiorAt(1.0f, 1.0f, 1.0f);
    REQUIRE(inside);
    CHECK(inside->ambient == glm::vec3(0.3f, 0.2f, 0.1f));
    REQUIRE(inside->floorColor);
    CHECK(inside->floorColor->x == Catch::Approx(0.6f));
    // Anywhere in the world, not only near the player.
    CHECK_FALSE(world.interiorAt(30.0f, 30.0f, 1.0f));
}

TEST_CASE("a building out of the query's reach is not asked about", "[collision]") {
    // The camera's reach round the player keeps a query from walking every
    // building in the zone; everywhere means what it says.
    CollisionWorld world;
    world.placeWmo(1, building({group({floorAt(0.0f)})}), at({50, 50, 10}));

    CHECK_FALSE(world.wmoFloor(Reach::around({500, 500, 10}, 20.0f), 50.0f, 50.0f, 11.0f));
    CHECK(world.wmoFloor(Reach::around({55, 50, 10}, 20.0f), 50.0f, 50.0f, 11.0f) == Catch::Approx(10.0f));
    CHECK(world.wmoFloor(kEverywhere, 50.0f, 50.0f, 11.0f) == Catch::Approx(10.0f));
}

TEST_CASE("the terrain is a floor, has holes and names its area where its tile is loaded",
          "[collision]") {
    // One tile, every chunk flat at height 7, and one quad cut out.
    constexpr float kTile = wowee::core::coords::TILE_SIZE;
    constexpr float kChunk = kTile / 16.0f;
    const auto [tileX, tileY] = wowee::core::coords::worldToTile(110.0f, 210.0f);
    const float maxX = (32 - tileY) * kTile;
    const float maxY = (32 - tileX) * kTile;

    wowee::pipeline::ADTTerrain terrain;
    terrain.loaded = true;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            auto& chunk = terrain.getChunk(cx, cy);
            chunk.position[0] = maxX - cy * kChunk;
            chunk.position[1] = maxY - cx * kChunk;
            chunk.position[2] = 7.0f;
            chunk.heightMap.heights.fill(0.0f);
            chunk.heightMap.loaded = true;
            chunk.areaId = 1519;
        }
    }
    // The chunk holding (110, 210) and its quad (5, 5).
    const int cy = static_cast<int>((maxX - 110.0f) / kChunk);
    const int cx = static_cast<int>((maxY - 210.0f) / kChunk);
    terrain.getChunk(cx, cy).holes = 1 << ((5 / 2) * 4 + (5 / 2));

    CollisionWorld world;
    CHECK_FALSE(world.hasTerrainAt(110.0f, 210.0f));
    world.addTerrainTile(TerrainTileHeights::build(terrain, tileX, tileY));

    CHECK(world.hasTerrainAt(110.0f, 210.0f));
    CHECK(world.terrainHeight(120.0f, 200.0f) == Catch::Approx(7.0f));
    CHECK(world.areaIdAt(120.0f, 200.0f) == 1519u);
    CHECK(world.terrainHoleAt(110.0f, 210.0f));
    CHECK(world.terrainChunkHasHoles(110.0f, 210.0f));
    // The same chunk, another quad: ground.
    const float chunkMaxX = maxX - cy * kChunk;
    CHECK_FALSE(world.terrainHoleAt(chunkMaxX - 0.5f, 210.0f));
    CHECK(world.terrainChunkHasHoles(chunkMaxX - 0.5f, 210.0f));

    world.removeTerrainTile(tileX, tileY);
    CHECK_FALSE(world.hasTerrainAt(110.0f, 210.0f));
    CHECK_FALSE(world.terrainHeight(120.0f, 200.0f));
}

TEST_CASE("leaving the map leaves nothing to stand on", "[collision]") {
    CollisionWorld world;
    world.placeM2(1, deck(), at({0, 0, 0}), 1.0f);
    world.placeWmo(2, building({group({floorAt(0.0f)})}), at({100, 0, 0}));
    world.clear();

    CHECK_FALSE(world.m2Floor(kEverywhere, 0, 0, 1));
    CHECK_FALSE(world.wmoFloor(kEverywhere, 100, 0, 1));
    CHECK(world.m2Count() == 0);
    CHECK(world.wmoCount() == 0);
}
