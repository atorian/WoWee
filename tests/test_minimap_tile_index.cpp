// Which file the minimap and the distant terrain draw a map tile from. The
// lines are as the 3.3.5a client's md5translate.trs has them: both tile
// indices two digits, which is how the client asks for them too
// ("%s\map%d_%02d.blp").
#include <catch_amalgamated.hpp>
#include "pipeline/minimap_tile_index.hpp"
#include <string_view>
#include <vector>
using wowee::pipeline::MinimapTileIndex;
namespace {
MinimapTileIndex index(std::string_view trs) {
    std::vector<uint8_t> bytes(trs.begin(), trs.end());
    return MinimapTileIndex::parse(bytes);
}
constexpr std::string_view kTrs =
    "dir: Expansion01\r\n"
    "Expansion01\\map42_06.blp\t67ba43d493e62a8fad5de319e6d4cb05.blp\r\n"
    "dir: Northrend\r\n"
    "Northrend\\map30_21.blp\t02753ca37edefda6dcd7b5ba58af05cc.blp\r\n";
}
TEST_CASE("a tile in the first ten rows of a map is found", "[minimap][tiles]") {
    const auto tiles = index(kTrs);
    const auto* file = tiles.terrainTile("Expansion01", 42, 6);
    REQUIRE(file != nullptr);
    CHECK(*file == "67ba43d493e62a8fad5de319e6d4cb05");
}
TEST_CASE("a tile is found whatever case the map directory is given in", "[minimap][tiles]") {
    const auto tiles = index(kTrs);
    const auto* file = tiles.terrainTile("NORTHREND", 30, 21);
    REQUIRE(file != nullptr);
    CHECK(*file == "02753ca37edefda6dcd7b5ba58af05cc");
}
TEST_CASE("a tile the index does not list has no file", "[minimap][tiles]") {
    const auto tiles = index(kTrs);
    CHECK(tiles.terrainTile("Northrend", 30, 22) == nullptr);
    CHECK(tiles.terrainTile("Azeroth", 30, 21) == nullptr);
    CHECK(index("").terrainTile("Northrend", 30, 21) == nullptr);
}
