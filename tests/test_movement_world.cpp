// The map as the original client's movement collects it, fed the way the
// client feeds it: terrain tiles through CollisionWorld, buildings as the WMO
// renderer builds them, doodad models from the one collision model cache -
// offered by whoever parsed them, or read by the cache itself. What a mover is answered - not ready, failed, or the
// facets - for doodads and buildings the map data places.
#include <catch_amalgamated.hpp>

#include "collision/collision_world.hpp"
#include "collision/model_collision_cache.hpp"
#include "collision/movement_world.hpp"
#include "collision/terrain_heights.hpp"
#include "pipeline/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"

#include <glm/glm.hpp>

#include <cstring>
#include <map>

using namespace wowee;
using collision::MovementGeometryStatus;

namespace {

constexpr uint32_t kStaticDoodads = 0x1;

/// A placement in ADT file space for a point in world (render) space.
void placeAt(float position[3], const glm::vec3& world) {
    const glm::vec3 adt = core::coords::worldToAdt(world);
    position[0] = adt.x;
    position[1] = adt.y;
    position[2] = adt.z;
}

/// Tile (32, 32): its chunk (0, 0) spans x and y from -33.3 to 0. One MDDF
/// doodad of `model` stands at (-10, -10, 0), listed by chunk (0, 0).
std::shared_ptr<const collision::TerrainTileHeights> tileWithDoodad(
        const std::string& model, uint32_t uniqueId, int tileX = 32) {
    auto terrain = std::make_unique<pipeline::ADTTerrain>();
    terrain->doodadNames = {model};
    pipeline::ADTTerrain::DoodadPlacement doodad{};
    doodad.nameId = 0;
    doodad.uniqueId = uniqueId;
    placeAt(doodad.position, {-10.0f, -10.0f, 0.0f});
    doodad.scale = 1024;
    terrain->doodadPlacements = {doodad};
    auto& chunk = terrain->chunks[0];
    chunk.loaded = true;
    chunk.heightMap.loaded = true;
    chunk.heightMap.heights.fill(0.0f);
    chunk.doodadReferences = {0};
    return collision::TerrainTileHeights::build(*terrain, tileX, 32);
}

/// A parsed model with one collision triangle 1 yard up.
pipeline::M2Model raisedTriangle() {
    pipeline::M2Model model{};
    model.collisionVertices = {{-2, -2, 1}, {2, -2, 1}, {0, 2, 1}};
    model.collisionIndices = {0, 1, 2};
    model.collisionNormals = {{0, 0, 1}};
    model.boundMin = {-2, -2, 0};
    model.boundMax = {2, 2, 2};
    return model;
}

/// The same triangle as an M2 file: MD20, version 264, the collision
/// indices, vertices and normals in the header's collision arrays.
std::vector<uint8_t> raisedTriangleFile() {
    std::vector<uint8_t> bytes(352 + 12, 0);
    auto put32 = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    std::memcpy(bytes.data(), "MD20", 4);
    put32(4, 264);
    put32(0xd8, 3); put32(0xdc, 340);
    put32(0xe0, 3); put32(0xe4, 304);
    put32(0xe8, 1); put32(0xec, 352);
    const float positions[] = {-2, -2, 1, 2, -2, 1, 0, 2, 1};
    const uint16_t indices[] = {0, 1, 2};
    const float normal[] = {0, 0, 1};
    std::memcpy(bytes.data() + 304, positions, sizeof(positions));
    std::memcpy(bytes.data() + 340, indices, sizeof(indices));
    std::memcpy(bytes.data() + 352, normal, sizeof(normal));
    return bytes;
}

/// The data, as the cache reads files nobody offered: these files and no
/// others, read at once rather than on a worker.
struct Files final : collision::ModelFileSource {
    std::map<std::string, std::vector<uint8_t>> files;
    std::shared_ptr<const std::vector<uint8_t>> read(const std::string& path) const override {
        const auto found = files.find(path);
        if (found == files.end()) return nullptr;
        return std::make_shared<const std::vector<uint8_t>>(found->second);
    }
    void runInBackground(std::function<void()> job) const override { job(); }
};

collision::MovementFacetRequest aroundDoodad() {
    collision::MovementFacetRequest request{};
    request.bodyMin = {-11, -11, 0};
    request.bodyMax = {-9, -9, 2};
    request.queryMin = {-12, -12, -1};
    request.queryMax = {-8, -8, 3};
    return request;
}

size_t facetsAtHeight(const std::vector<collision::MovementFacet>& facets, float z) {
    size_t count = 0;
    for (const auto& facet : facets) {
        bool all = true;
        for (const auto& v : facet.vertices) all = all && std::abs(v.z - z) < 1e-4f;
        count += all;
    }
    return count;
}

std::bitset<64 * 64> terrainAt(std::initializer_list<int> tileXs) {
    std::bitset<64 * 64> tiles;
    for (int x : tileXs) tiles.set(32 * 64 + x);
    return tiles;
}

}  // namespace

TEST_CASE("a tile's doodad is solid once its model arrives, and the area waits for it",
          "[collision][movement_world]") {
    collision::CollisionWorld world;
    world.movement().beginMap(0, terrainAt({32}));
    world.addTerrainTile(tileWithDoodad("World\\Tree.mdx", 7));
    std::vector<collision::MovementFacet> facets;

    CHECK(world.movement().query(aroundDoodad(), kStaticDoodads, facets) == MovementGeometryStatus::NotReady);

    (void)world.models().offer("world/tree.m2", raisedTriangle());
    REQUIRE(world.movement().query(aroundDoodad(), kStaticDoodads, facets) == MovementGeometryStatus::Ready);
    CHECK(facetsAtHeight(facets, 1.0f) == 1);
}

TEST_CASE("a doodad whose model cannot be loaded fails its area", "[collision][movement_world]") {
    collision::CollisionWorld world;
    world.movement().beginMap(0, terrainAt({32}));
    world.models().setSource(std::make_shared<Files>());
    world.addTerrainTile(tileWithDoodad("World\\Missing.m2", 7));
    std::vector<collision::MovementFacet> facets;
    CHECK(world.movement().query(aroundDoodad(), kStaticDoodads, facets) == MovementGeometryStatus::Failed);
}

TEST_CASE("a doodad two tiles place is one placement, kept while either tile is loaded",
          "[collision][movement_world]") {
    collision::CollisionWorld world;
    world.movement().beginMap(0, terrainAt({32, 33}));
    (void)world.models().offer("world/tree.m2", raisedTriangle());
    world.addTerrainTile(tileWithDoodad("World\\Tree.mdx", 7, 32));
    world.addTerrainTile(tileWithDoodad("World\\Tree.mdx", 7, 33));
    world.removeTerrainTile(33, 32);
    std::vector<collision::MovementFacet> facets;
    REQUIRE(world.movement().query(aroundDoodad(), kStaticDoodads, facets) == MovementGeometryStatus::Ready);
    CHECK(facetsAtHeight(facets, 1.0f) == 1);
}

TEST_CASE("a building's group doodads of its placement's set are solid with it",
          "[collision][movement_world]") {
    collision::CollisionWorld world;
    world.movement().beginMap(0, {});
    // The building's own groups need no terrain: a map that is one building.
    world.movement().beginBuildingMap(0, "World\\Hall.wmo", glm::mat4(1.0f), 1);
    std::vector<collision::MovementFacet> facets;
    collision::MovementFacetRequest request{};
    request.bodyMin = {9, 9, 0};
    request.bodyMax = {11, 11, 2};
    request.queryMin = {8, 8, -1};
    request.queryMax = {12, 12, 3};

    CHECK(world.movement().query(request, kStaticDoodads, facets) == MovementGeometryStatus::NotReady);

    // One group, 0..20 on each axis. MODD: doodad 0 in set 0 (every
    // placement's), doodad 1 in set 1 (this placement's), doodad 2 in set 2
    // (another's). The group's MODR names all three.
    pipeline::WMOModel input;
    input.sourcePath = "World/Hall.wmo";
    input.boundingBoxMin = {0, 0, 0};
    input.boundingBoxMax = {20, 20, 20};
    input.groups.resize(1);
    input.groupInfo.resize(1);
    input.groupInfo[0].boundingBoxMin = input.groups[0].boundingBoxMin = {0, 0, 0};
    input.groupInfo[0].boundingBoxMax = input.groups[0].boundingBoxMax = {20, 20, 20};
    input.groups[0].loadState = pipeline::WmoGroupLoadState::Ready;
    input.groups[0].doodadRefs = {0, 1, 2};
    input.doodadNames = {{0, "World\\Floor.mdx"}, {10, "World\\Lamp.mdx"}, {20, "World\\Other.mdx"}};
    for (const auto& [name, height] : {std::pair{0u, 2.0f}, {10u, 3.0f}, {20u, 4.0f}}) {
        pipeline::WMODoodad doodad{};
        doodad.nameIndex = name;
        doodad.position = {10, 10, height - 1.0f};
        doodad.rotation = glm::quat(1, 0, 0, 0);
        doodad.scale = 1.0f;
        input.doodads.push_back(doodad);
    }
    input.doodadSets = {{"Set_$DefaultGlobal", 0, 1, 0}, {"Lamps", 1, 1, 0}, {"Other", 2, 1, 0}};
    auto shape = std::make_shared<collision::WmoShape>();
    shape->boundingBoxMin = input.boundingBoxMin;
    shape->boundingBoxMax = input.boundingBoxMax;
    shape->originalGroups.push_back(collision::WmoModelGroup::build(
        input, 0, collision::WmoGroupShape::build(input.groups[0])));
    shape->describeDoodads(input);
    world.movement().addWmoModel(shape);
    for (const char* model : {"world/floor.m2", "world/lamp.m2", "world/other.m2"}) {
        (void)world.models().offer(model, raisedTriangle());
    }

    REQUIRE(world.movement().query(request, kStaticDoodads, facets) == MovementGeometryStatus::Ready);
    CHECK(facetsAtHeight(facets, 2.0f) == 1);
    CHECK(facetsAtHeight(facets, 3.0f) == 1);
    CHECK(facetsAtHeight(facets, 4.0f) == 0);
}

TEST_CASE("a doodad model nobody has parsed is read by the cache and is solid",
          "[collision][movement_world]") {
    collision::CollisionWorld world;
    auto files = std::make_shared<Files>();
    files->files["world\\tree.m2"] = raisedTriangleFile();
    world.models().setSource(files);
    world.movement().beginMap(0, terrainAt({32}));
    world.addTerrainTile(tileWithDoodad("World\\Tree.mdx", 7));
    std::vector<collision::MovementFacet> facets;
    REQUIRE(world.movement().query(aroundDoodad(), kStaticDoodads, facets) == MovementGeometryStatus::Ready);
    CHECK(facetsAtHeight(facets, 1.0f) == 1);
}

TEST_CASE("a model's collision is built once, whoever offers it", "[collision][movement_world]") {
    collision::ModelCollisionCache models;
    const auto first = models.offer("World\\Tree.mdx", raisedTriangle());
    const auto second = models.offer("world/tree.m2", raisedTriangle());
    CHECK(first == second);
    CHECK(models.find("WORLD\\TREE.M2").collision == first);
}
