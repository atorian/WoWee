// What the minimap draws when the player is in a building, by the 3.3.5a
// client's rules (wow.exe: mode 0x75d6c2, gather box 0x75d81a, group walk
// 0x71e610, tiles 0x71e090). The expected values are worked out by hand from
// those rules, not by the code under test.
#include <catch_amalgamated.hpp>
#include "rendering/wmo_minimap.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>

using namespace wowee::rendering;

namespace {

using Group = WmoMinimapModel::Group;

Group box(glm::vec3 lo, glm::vec3 hi, uint32_t flags) {
    return {.boundsMin = lo, .boundsMax = hi, .flags = flags};
}

/// A doorway: a square portal standing at x, spanning y0..y1 and z0..z1.
void door(WmoMinimapModel& m, int a, int b, float x, float y0, float y1, float z0, float z1) {
    const auto first = static_cast<uint16_t>(m.portalVertices.size());
    m.portalVertices.insert(m.portalVertices.end(),
                            {{x, y0, z0}, {x, y1, z0}, {x, y1, z1}, {x, y0, z1}});
    const auto portal = static_cast<uint16_t>(m.portals.size());
    m.portals.push_back({first, 4});
    m.portalRefs.push_back({portal, static_cast<uint16_t>(b)});
    m.groups[a].portalRefCount++;
}

/// Refs must sit in group order, as in a file: lay them out per group.
WmoMinimapModel withRefs(WmoMinimapModel m, const std::vector<std::tuple<int, int, float, float, float, float, float>>& doors) {
    for (size_t g = 0; g < m.groups.size(); ++g) {
        m.groups[g].firstPortalRef = static_cast<uint16_t>(m.portalRefs.size());
        m.groups[g].portalRefCount = 0;
        for (const auto& [a, b, x, y0, y1, z0, z1] : doors) {
            if (a == static_cast<int>(g)) door(m, a, b, x, y0, y1, z0, z1);
        }
    }
    return m;
}

std::set<std::tuple<int, int, int>> named(const std::vector<WmoMinimapTile>& tiles) {
    std::set<std::tuple<int, int, int>> out;
    for (const auto& t : tiles) out.insert({t.group, t.x, t.y});
    return out;
}

const WmoMinimapBox kEverything{glm::vec3(-1000.0f), glm::vec3(1000.0f)};

/// A street (exterior, group 0) with a shop off it (interior, group 1)
/// through a door at x = 100.
WmoMinimapModel streetAndShop() {
    WmoMinimapModel m;
    m.groups = {box({0, 0, 0}, {100, 50, 20}, kWmoGroupExterior),
                box({100, 0, 0}, {140, 40, 20}, kWmoGroupInterior)};
    return withRefs(m, {{0, 1, 100.0f, 10.0f, 20.0f, 0.0f, 8.0f}, {1, 0, 100.0f, 10.0f, 20.0f, 0.0f, 8.0f}});
}

std::vector<uint8_t> wmoAreaTable(const std::vector<std::array<int32_t, 28>>& rows) {
    std::vector<uint8_t> bytes(20 + rows.size() * 28 * 4 + 1);
    std::memcpy(bytes.data(), "WDBC", 4);
    const uint32_t header[4] = {static_cast<uint32_t>(rows.size()), 28, 28 * 4, 1};
    std::memcpy(bytes.data() + 4, header, sizeof(header));
    for (size_t r = 0; r < rows.size(); ++r) std::memcpy(bytes.data() + 20 + r * 112, rows[r].data(), 112);
    return bytes;
}

std::array<int32_t, 28> areaRow(int32_t id, int32_t wmoId, int32_t nameSet, int32_t group, int32_t flags) {
    std::array<int32_t, 28> row{};
    row[0] = id; row[1] = wmoId; row[2] = nameSet; row[3] = group; row[9] = flags;
    return row;
}

}  // namespace

TEST_CASE("a building is cut into 128-yard tiles from its group's box minimum", "[minimap][wmo]") {
    // Dalaran's group 68 is 167.6 by 18.6 yards and has tiles 068_00_00 and
    // 068_01_00.
    WmoMinimapModel m;
    m.groups = {box({-256.7f, 24.3f, -72.9f}, {-89.1f, 42.9f, -12.5f}, kWmoGroupInterior)};
    const auto tiles = wmoMinimapTiles(m, 0, true, kEverything);
    REQUIRE(tiles.size() == 2);
    CHECK(named(tiles) == std::set<std::tuple<int, int, int>>{{0, 0, 0}, {0, 1, 0}});
    const auto second = std::find_if(tiles.begin(), tiles.end(), [](const auto& t) { return t.x == 1; });
    CHECK(second->origin.x == Catch::Approx(-128.7f));
    CHECK(second->origin.y == Catch::Approx(24.3f));
}

TEST_CASE("outdoors the walk goes through the door into the shop", "[minimap][wmo]") {
    const auto m = streetAndShop();
    CHECK(named(wmoMinimapTiles(m, 0, false, kEverything)) ==
          std::set<std::tuple<int, int, int>>{{0, 0, 0}, {1, 0, 0}});
}

TEST_CASE("indoors the street outside is not drawn", "[minimap][wmo]") {
    const auto m = streetAndShop();
    CHECK(named(wmoMinimapTiles(m, 1, true, kEverything)) == std::set<std::tuple<int, int, int>>{{1, 0, 0}});
}

TEST_CASE("indoors the walk does not cross the street to the shop opposite", "[minimap][wmo]") {
    WmoMinimapModel m;
    m.groups = {box({0, 0, 0}, {40, 40, 10}, kWmoGroupInterior),
                box({40, 0, 0}, {80, 40, 10}, kWmoGroupExterior),
                box({80, 0, 0}, {120, 40, 10}, kWmoGroupInterior)};
    m = withRefs(m, {{0, 1, 40.0f, 10.0f, 20.0f, 0.0f, 8.0f}, {1, 2, 80.0f, 10.0f, 20.0f, 0.0f, 8.0f}});
    CHECK(named(wmoMinimapTiles(m, 0, true, kEverything)) == std::set<std::tuple<int, int, int>>{{0, 0, 0}});
}

TEST_CASE("a door wholly outside the gathered box leads nowhere", "[minimap][wmo]") {
    const auto m = streetAndShop();
    // The street reaches into the box; the door at x = 100 does not.
    const WmoMinimapBox nearStart{glm::vec3(-50, -50, -50), glm::vec3(60, 60, 50)};
    CHECK(named(wmoMinimapTiles(m, 0, false, nearStart)) == std::set<std::tuple<int, int, int>>{{0, 0, 0}});
}

TEST_CASE("nothing above the top of the player's own group is reached", "[minimap][wmo]") {
    WmoMinimapModel m;
    m.groups = {box({0, 0, 0}, {40, 40, 10}, kWmoGroupInterior),
                box({0, 0, 15}, {40, 40, 30}, kWmoGroupInterior)};
    // A stairwell opening at 12..14, above the lower room's top at 10.
    m = withRefs(m, {{0, 1, 20.0f, 0.0f, 10.0f, 12.0f, 14.0f}});
    CHECK(named(wmoMinimapTiles(m, 0, true, kEverything)) == std::set<std::tuple<int, int, int>>{{0, 0, 0}});
}

TEST_CASE("an unreachable group is walked through but not drawn", "[minimap][wmo]") {
    WmoMinimapModel m;
    m.groups = {box({0, 0, 0}, {40, 40, 10}, kWmoGroupInterior),
                box({40, 0, 0}, {80, 40, 10}, kWmoGroupInterior | kWmoGroupUnreachable),
                box({80, 0, 0}, {120, 40, 10}, kWmoGroupInterior)};
    m = withRefs(m, {{0, 1, 40.0f, 10.0f, 20.0f, 0.0f, 8.0f}, {1, 2, 80.0f, 10.0f, 20.0f, 0.0f, 8.0f}});
    CHECK(named(wmoMinimapTiles(m, 0, true, kEverything)) ==
          std::set<std::tuple<int, int, int>>{{0, 0, 0}, {2, 0, 0}});
}

TEST_CASE("tiles are gathered over the player's radius cell widened by a radius", "[minimap][wmo]") {
    // Dalaran's centre at the widest indoor zoom, 150 yards: the cell is
    // 5700..5850 by 600..750.
    const auto b = wmoMinimapGatherBox(glm::vec3(5804.0f, 624.0f, 646.0f), 150.0f);
    CHECK(b.min.x == Catch::Approx(5550.0f));
    CHECK(b.min.y == Catch::Approx(450.0f));
    CHECK(b.max.x == Catch::Approx(6000.0f));
    CHECK(b.max.y == Catch::Approx(900.0f));
    CHECK(b.min.z == Catch::Approx(421.0f));
    CHECK(b.max.z == Catch::Approx(796.0f));
}

TEST_CASE("the player stands in the interior group before the street around it", "[minimap][wmo]") {
    const auto m = streetAndShop();
    WmoMinimapModel inShop = m;
    inShop.groups[0].boundsMax.x = 150.0f;  // the street's box takes in the shop too
    CHECK(inShop.groupAt({120, 20, 5}) == 1);
    CHECK(inShop.groupAt({50, 20, 5}) == 0);
    CHECK(inShop.groupAt({50, 20, 50}) == -1);
}

TEST_CASE("a building whose WMOAreaTable row has flag 1 is drawn from its own tiles outdoors", "[minimap][wmo]") {
    // Dalaran: WMOID 5164, its building row (WMOGroupID -1) has Flags 9; a
    // room row of it has none. Stormwind (61) has no flag.
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(wmoAreaTable({areaRow(43381, 5164, 0, -1, 9), areaRow(43382, 5164, 0, 5, 0),
                                   areaRow(100, 61, 0, -1, 0)})));
    const auto table = WmoAreaFlags::read(dbc, nullptr);
    REQUIRE(table);
    CHECK(wmoMinimapShown(false, table->rootFlags(5164, 0)));
    CHECK_FALSE(wmoMinimapShown(false, table->rootFlags(61, 0)));
    CHECK_FALSE(wmoMinimapShown(false, table->rootFlags(5164, 7)));
    CHECK(wmoMinimapShown(true, table->rootFlags(61, 0)));
}

TEST_CASE("a WMOAreaTable of another shape is not read by guesswork", "[minimap][wmo]") {
    std::vector<uint8_t> bytes(20 + 4 * 10 + 1);
    std::memcpy(bytes.data(), "WDBC", 4);
    const uint32_t header[4] = {1, 10, 40, 1};
    std::memcpy(bytes.data() + 4, header, sizeof(header));
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));
    CHECK_FALSE(WmoAreaFlags::read(dbc, nullptr).has_value());
}

TEST_CASE("a building's tiles are filed under its path without World and .wmo", "[minimap][wmo]") {
    wowee::pipeline::WMOModel wmo;
    wmo.sourcePath = "World\\wmo\\Northrend\\Dalaran\\ND_Dalaran.wmo";
    wmo.wmoId = 5164;
    const auto m = WmoMinimapModel::from(wmo);
    REQUIRE(m);
    CHECK(m->tileStem == "wmo\\Northrend\\Dalaran\\ND_Dalaran");
    CHECK(m->wmoId == 5164);
}

TEST_CASE("a building tile image runs along +X and up +Y from its origin at two pixels a yard", "[minimap][wmo]") {
    // A 256x128 image covers 128 by 64 yards. The composite covers
    // -300..600 both ways; u grows east (render -x), v south (render -y).
    const WmoMinimapBox area{glm::vec3(-300, -300, 0), glm::vec3(600, 600, 0)};
    const WmoMinimapTile tile{.group = 0, .x = 0, .y = 0, .origin = {0, 0}, .midZ = 0};
    const auto q = wmoMinimapQuad(tile, 256, 128, glm::mat4(1.0f), area);
    CHECK(q.corner.x == Catch::Approx(0.666667f).margin(1e-5));   // top-left is model (0, 64)
    CHECK(q.corner.y == Catch::Approx(0.595556f).margin(1e-5));
    CHECK(q.down.x == Catch::Approx(0.0f).margin(1e-5));           // down to (0, 0)
    CHECK(q.down.y == Catch::Approx(0.071111f).margin(1e-5));
    CHECK(q.across.x == Catch::Approx(-0.142222f).margin(1e-5));  // across to (128, 64)
    CHECK(q.across.y == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("a building tile turns with the building's placement", "[minimap][wmo]") {
    // A quarter turn: model (x, y) stands at world (-y, x).
    const WmoMinimapBox area{glm::vec3(-300, -300, 0), glm::vec3(600, 600, 0)};
    glm::mat4 quarter(0.0f);
    quarter[0] = glm::vec4(0, 1, 0, 0);
    quarter[1] = glm::vec4(-1, 0, 0, 0);
    quarter[2] = glm::vec4(0, 0, 1, 0);
    quarter[3] = glm::vec4(0, 0, 0, 1);
    const WmoMinimapTile tile{.group = 0, .x = 0, .y = 0, .origin = {0, 0}, .midZ = 0};
    const auto q = wmoMinimapQuad(tile, 256, 128, quarter, area);
    CHECK(q.corner.x == Catch::Approx(0.737778f).margin(1e-5));   // world (-64, 0)
    CHECK(q.corner.y == Catch::Approx(0.666667f).margin(1e-5));
    CHECK(q.down.x == Catch::Approx(-0.071111f).margin(1e-5));
    CHECK(q.down.y == Catch::Approx(0.0f).margin(1e-5));
    CHECK(q.across.x == Catch::Approx(0.0f).margin(1e-5));
    CHECK(q.across.y == Catch::Approx(-0.142222f).margin(1e-5));
}

TEST_CASE("the player's own group is drawn last and the others from the lowest up", "[minimap][wmo]") {
    std::vector<WmoMinimapTile> tiles = {{.group = 3, .midZ = 0}, {.group = 1, .midZ = 20},
                                         {.group = 2, .midZ = -5}, {.group = 4, .midZ = 5}};
    wmoMinimapDrawOrder(tiles, 3, 0.0f);
    std::vector<int> order;
    for (const auto& t : tiles) order.push_back(t.group);
    CHECK(order == std::vector<int>{2, 4, 1, 3});
}
