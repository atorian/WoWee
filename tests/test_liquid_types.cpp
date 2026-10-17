// What each liquid in the map data is: LiquidType.dbc read through its column
// layout, the client's rule for a WMO group's liquid, MCLQ's ocean flag - and
// what the map's liquid then says it is to the camera, swimming and drawing.
//
// The ids were turned into a kind with (id - 1) % 4, which holds for the
// first twenty rows only: "Naxxramas - Slime" (21) came out as water. And
// MCLQ wrote the kind itself into the same field, so its ocean read as a lake.
#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/liquid_types.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/liquid_scene.hpp"

#include <cstring>
#include <string>
#include <vector>

using wowee::pipeline::ADTLoader;
using wowee::pipeline::ADTTerrain;
using wowee::pipeline::DBCFieldMap;
using wowee::pipeline::DBCFile;
using wowee::pipeline::LiquidBasic;
using wowee::pipeline::LiquidTypeTable;
using wowee::pipeline::wmoGroupLiquidType;
using wowee::pipeline::WMOLiquid;
using wowee::rendering::LiquidScene;

namespace {

constexpr uint32_t kFields = 16;
constexpr uint32_t kTextureCol = 15;

struct Row {
    uint32_t id;
    const char* name;
    uint32_t type;
    const char* texture;
};

/// A LiquidType.dbc of these rows, in the WotLK columns the layout names.
std::vector<uint8_t> liquidTypeDbc(const std::vector<Row>& rows) {
    std::string strings(1, '\0');
    auto intern = [&](const char* s) {
        const uint32_t at = static_cast<uint32_t>(strings.size());
        strings += s;
        strings.push_back('\0');
        return at;
    };
    std::vector<uint32_t> records;
    for (const Row& r : rows) {
        std::vector<uint32_t> rec(kFields, 0);
        rec[0] = r.id;
        rec[1] = intern(r.name);
        rec[3] = r.type;
        rec[14] = 1;
        rec[kTextureCol] = r.texture ? intern(r.texture) : 0;
        records.insert(records.end(), rec.begin(), rec.end());
    }
    std::vector<uint8_t> out(20);
    std::memcpy(out.data(), "WDBC", 4);
    const uint32_t header[4] = {static_cast<uint32_t>(rows.size()), kFields, kFields * 4,
                                static_cast<uint32_t>(strings.size())};
    std::memcpy(out.data() + 4, header, sizeof(header));
    const auto* bytes = reinterpret_cast<const uint8_t*>(records.data());
    out.insert(out.end(), bytes, bytes + records.size() * 4);
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

DBCFieldMap wotlkLayout() {
    DBCFieldMap map;
    map.fields = {{"ID", 0}, {"Name", 1}, {"Type", 3}, {"MaterialID", 14}, {"Texture", 15}};
    return map;
}

LiquidTypeTable tableOf(const std::vector<Row>& rows) {
    DBCFile dbc;
    REQUIRE(dbc.load(liquidTypeDbc(rows)));
    const DBCFieldMap layout = wotlkLayout();
    return LiquidTypeTable::fromDbc(dbc, &layout, "test LiquidType.dbc");
}

const std::vector<Row> kStockRows = {
    {1, "Water", 0, "XTextures\\river\\lake_a.%d.blp"},
    {2, "Ocean", 1, "XTextures\\ocean\\ocean_h.%d.blp"},
    {3, "Magma", 2, "XTextures\\lava\\lava.%d.blp"},
    {4, "Slime", 3, "XTextures\\slime\\slime.%d.blp"},
    {13, "WMO Water", 0, "XTextures\\river\\lake_a.%d.blp"},
    {14, "WMO Ocean", 1, "XTextures\\ocean\\ocean_h.%d.blp"},
    {19, "WMO Magma", 2, "XTextures\\lava\\lava.%d.blp"},
    {21, "Naxxramas - Slime", 3, "XTextures\\slime\\slime.%d.blp"},
};

constexpr float kTile = 533.33333f;
constexpr float kChunk = kTile / 16.0f;

ADTTerrain tile() {
    ADTTerrain t;
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            t.getChunk(cx, cy).position[0] = 16.0f * kTile - static_cast<float>(cy) * kChunk;
            t.getChunk(cx, cy).position[1] = 16.0f * kTile - static_cast<float>(cx) * kChunk;
        }
    return t;
}

void flood(ADTTerrain& t, uint16_t type) {
    ADTTerrain::WaterLayer layer{};
    layer.liquidType = type;
    layer.minHeight = layer.maxHeight = 10.0f;
    layer.width = layer.height = 8;
    layer.mask.assign(8, 0xFF);
    t.waterData[0].layers.push_back(layer);
}

constexpr float kMidX = 16.0f * kTile - 0.5f * kChunk;
constexpr float kMidY = 16.0f * kTile - 0.5f * kChunk;

}  // namespace

TEST_CASE("a liquid is what its LiquidType row says, past the first twenty rows too", "[liquid]") {
    const LiquidTypeTable table = tableOf(kStockRows);
    LiquidScene scene;
    scene.setLiquidTypes(&table);
    ADTTerrain t = tile();
    flood(t, 21);
    scene.addTerrainTile(t, 16, 16);

    CHECK(scene.basicTypeAt(kMidX, kMidY) == LiquidBasic::Slime);
    REQUIRE(scene.bodyAt(kMidX, kMidY));
    CHECK(scene.bodyAt(kMidX, kMidY)->basicType == static_cast<uint8_t>(LiquidBasic::Slime));
}

TEST_CASE("a row names its animated texture and material", "[liquid]") {
    const LiquidTypeTable table = tableOf(kStockRows);
    REQUIRE(table.find(2));
    CHECK(table.find(2)->texturePattern == "XTextures\\ocean\\ocean_h.%d.blp");
    CHECK(table.find(2)->name == "Ocean");
    CHECK(table.find(2)->materialId == 1);
}

TEST_CASE("a row of no basic kind is left out, and its liquid taken for water", "[liquid]") {
    const LiquidTypeTable table = tableOf({{1, "Water", 0, nullptr}, {40, "Broken", 7, nullptr}});
    CHECK_FALSE(table.find(40));
    CHECK(table.basicOf(40) == LiquidBasic::Water);
    CHECK(table.basicOf(1234) == LiquidBasic::Water);
}

TEST_CASE("a layout without the type column falls back to the four basic rows", "[liquid]") {
    DBCFile dbc;
    REQUIRE(dbc.load(liquidTypeDbc(kStockRows)));
    DBCFieldMap layout;
    layout.fields = {{"ID", 0}};
    const LiquidTypeTable table = LiquidTypeTable::fromDbc(dbc, &layout, "test");
    CHECK(table.basicOf(2) == LiquidBasic::Ocean);
    CHECK(table.basicOf(4) == LiquidBasic::Slime);
    REQUIRE(table.find(2));
    CHECK(table.find(2)->texturePattern.empty());
}

TEST_CASE("a building's liquid is what its group header says, by the client's rule", "[liquid]") {
    const LiquidTypeTable table = tableOf(kStockRows);
    constexpr uint32_t kOceanFlag = 0x80000;

    SECTION("a model naming table ids: a basic one goes through the WMO rows") {
        CHECK(table.basicOf(wmoGroupLiquidType(1, 0, true, 0)) == LiquidBasic::Water);
        CHECK(table.basicOf(wmoGroupLiquidType(1, kOceanFlag, true, 0)) == LiquidBasic::Ocean);
        CHECK(table.basicOf(wmoGroupLiquidType(3, 0, true, 0)) == LiquidBasic::Magma);
    }
    SECTION("a model naming table ids: past the basic rows, the id itself") {
        CHECK(table.basicOf(wmoGroupLiquidType(21, 0, true, 0)) == LiquidBasic::Slime);
    }
    SECTION("an older model names the kind") {
        CHECK(table.basicOf(wmoGroupLiquidType(0, 0, false, 0)) == LiquidBasic::Water);
        CHECK(table.basicOf(wmoGroupLiquidType(1, 0, false, 0)) == LiquidBasic::Ocean);
        CHECK(table.basicOf(wmoGroupLiquidType(2, 0, false, 0)) == LiquidBasic::Magma);
    }
    SECTION("an older model's green lava is what its tiles say") {
        CHECK(table.basicOf(wmoGroupLiquidType(15, 0, false, 2)) == LiquidBasic::Magma);
        CHECK(table.basicOf(wmoGroupLiquidType(15, 0, false, 0)) == LiquidBasic::Water);
    }
}

TEST_CASE("the sea a building's group says is the sea in the scene", "[liquid]") {
    const LiquidTypeTable table = tableOf(kStockRows);
    LiquidScene scene;
    scene.setLiquidTypes(&table);
    WMOLiquid pool;
    pool.xVerts = pool.yVerts = 3;
    pool.xTiles = pool.yTiles = 2;
    pool.basePosition = glm::vec3(0.0f, 0.0f, 0.0f);
    pool.materialId = 7;  // MOMT index: nothing to do with the liquid
    pool.liquidType = wmoGroupLiquidType(1, 0x80000, true, 0);
    REQUIRE(scene.addWmoLiquid(pool, glm::mat4(1.0f), 5));
    CHECK(scene.basicTypeAt(4.0f, 4.0f) == LiquidBasic::Ocean);
}

TEST_CASE("an MCLQ ocean is the sea, not a lake", "[liquid]") {
    // One MCNK flagged ocean (0x08) with a full MCLQ: 8 bytes of heights, the
    // 9x9 vertices and the 8x8 tiles, every tile wet.
    std::vector<uint8_t> mcnk(128, 0);
    const uint32_t flags = 0x08, ofsLiquid = 128, sizeLiquid = 720;
    std::memcpy(mcnk.data() + 0, &flags, 4);
    std::memcpy(mcnk.data() + 0x60, &ofsLiquid, 4);
    std::memcpy(mcnk.data() + 0x64, &sizeLiquid, 4);
    mcnk.resize(128 + 720, 0);
    std::vector<uint8_t> adt;
    const uint32_t magic = 0x4D434E4B, size = static_cast<uint32_t>(mcnk.size());
    adt.resize(8);
    std::memcpy(adt.data(), &magic, 4);
    std::memcpy(adt.data() + 4, &size, 4);
    adt.insert(adt.end(), mcnk.begin(), mcnk.end());

    const ADTTerrain loaded = ADTLoader::load(adt);
    REQUIRE(loaded.waterData[0].hasWater());

    const LiquidTypeTable table = tableOf(kStockRows);
    LiquidScene scene;
    scene.setLiquidTypes(&table);
    ADTTerrain t = tile();
    t.waterData[0] = loaded.waterData[0];
    scene.addTerrainTile(t, 16, 16);
    CHECK(scene.basicTypeAt(kMidX, kMidY) == LiquidBasic::Ocean);
}
