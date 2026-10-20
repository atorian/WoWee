// A point is lit as WoW.exe lights it: by its map's global light, with the
// Light.dbc volumes it stands in laid over it, farthest first
// (light_volume_order.hpp).
//
// The renderer took the two heaviest volumes, renormalised their weights to
// sum to one and fell back to a light of its own where none reached. So a
// zone's light held whole to the last yard of its falloff and then switched:
// flown above Sholazar's volumes the sky went from its yellow aurora to a pale
// blue that no file has, and back, as the player rose and fell across the
// edge.
//
// Each case below is a map made up here - a global light and three volumes,
// each light a single colour - read by LightingManager as it reads the
// client's files, with the point put where the case needs it.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/lighting_manager.hpp"

#include "test_support.hpp"

#include <bit>
#include <vector>

using wowee::rendering::LightingManager;
using wowee::rendering::LIGHT_COORD_UNITS_PER_YARD;
using wowee::rendering::lightPositionToWorld;

namespace {

// LightParams rows, one per light, each lit in a colour of its own.
constexpr uint32_t GLOBAL_PARAMS = 1;
constexpr uint32_t ZONE_PARAMS = 2;     // a zone's volume, 100 yards whole, 200 to its edge
constexpr uint32_t HAMLET_PARAMS = 3;   // a smaller volume inside it, 30 yards off its centre
constexpr uint32_t SPECK_PARAMS = 4;    // a volume two yards across, which takes no part

uint32_t colourOf(uint32_t params) {
    switch (params) {
        case GLOBAL_PARAMS: return 0x2040c0;
        case ZONE_PARAMS: return 0xc0c020;
        case HAMLET_PARAMS: return 0x20c040;
        default: return 0xff00ff;
    }
}

glm::vec3 rgb(uint32_t packed) {
    return glm::vec3((packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff) / 255.0f;
}

// The zone's centre, in Light.dbc's coordinates.
constexpr float ZONE_X = 20000.0f;
constexpr float ZONE_Y = 20000.0f;
constexpr float HAMLET_OFFSET = 30.0f;  // yards, along the file's X
constexpr float SPECK_OFFSET = -50.0f;

std::vector<uint32_t> lightRow(uint32_t id, float x, float y, float inner, float outer,
                               uint32_t params) {
    std::vector<uint32_t> row(15, 0);
    row[0] = id;
    row[1] = 0;  // map
    row[2] = std::bit_cast<uint32_t>(x);
    row[3] = std::bit_cast<uint32_t>(0.0f);
    row[4] = std::bit_cast<uint32_t>(y);
    row[5] = std::bit_cast<uint32_t>(inner * LIGHT_COORD_UNITS_PER_YARD);
    row[6] = std::bit_cast<uint32_t>(outer * LIGHT_COORD_UNITS_PER_YARD);
    row[7] = row[8] = row[9] = row[10] = params;
    return row;
}

std::vector<uint8_t> lightDbc() {
    const float yard = LIGHT_COORD_UNITS_PER_YARD;
    return wowee::test::wdbcFile(15, {
        // The global light: at the origin, with a yard of extent, as Northrend's is.
        lightRow(1, 0.0f, 0.0f, 1.0f, 1.0f, GLOBAL_PARAMS),
        lightRow(2, ZONE_X, ZONE_Y, 100.0f, 200.0f, ZONE_PARAMS),
        lightRow(3, ZONE_X + HAMLET_OFFSET * yard, ZONE_Y, 20.0f, 40.0f, HAMLET_PARAMS),
        lightRow(4, ZONE_X + SPECK_OFFSET * yard, ZONE_Y, 1.0f, 2.0f, SPECK_PARAMS),
    }, std::string(1, '\0'));
}

std::vector<uint8_t> lightParamsDbc() {
    std::vector<std::vector<uint32_t>> rows;
    for (uint32_t id = GLOBAL_PARAMS; id <= SPECK_PARAMS; ++id) {
        rows.push_back({id, 0, 0, 0, 0, 0, 0, 0, 0});
    }
    return wowee::test::wdbcFile(9, rows, std::string(1, '\0'));
}

/// Every colour channel of each row, the same at every hour.
std::vector<uint8_t> lightIntBandDbc() {
    std::vector<std::vector<uint32_t>> rows;
    for (uint32_t params = GLOBAL_PARAMS; params <= SPECK_PARAMS; ++params) {
        for (uint32_t channel = 0; channel < 18; ++channel) {
            std::vector<uint32_t> row(34, 0);
            row[0] = (params - 1) * 18 + channel + 1;
            row[1] = 2;
            row[2] = 0;
            row[3] = 1440;
            row[18] = row[19] = colourOf(params);
            rows.push_back(row);
        }
    }
    return wowee::test::wdbcFile(34, rows, std::string(1, '\0'));
}

glm::vec3 worldOf(float dbcX, float dbcY) { return lightPositionToWorld(dbcX, dbcY, 0.0f); }

/// The sky's top colour a point at `pos` is lit with: one of the light's
/// colours that goes to the screen as the file has it.
glm::vec3 skyAt(const char* rootName, const glm::vec3& pos) {
    wowee::pipeline::AssetManager assets;
    LightingManager lighting;
    const auto root = wowee::test::makeDataRoot(rootName, {
        {"dbfilesclient\\light.dbc", lightDbc()},
        {"dbfilesclient\\lightparams.dbc", lightParamsDbc()},
        {"dbfilesclient\\lightintband.dbc", lightIntBandDbc()},
    });
    REQUIRE(assets.initialize(root.string()));
    REQUIRE(lighting.initialize(&assets));
    // Long enough for the light's smoothing to
    // settle on where the point is.
    for (int i = 0; i < 10; ++i) {
        lighting.update(0.25f, pos, 0, 0, 12.0f, 0.0f);
    }
    lighting.update(0.25f, pos, 0, 0, 12.0f, 0.0f);
    return lighting.getLightingParams().skyTopColor;
}

void requireColour(const glm::vec3& got, const glm::vec3& want) {
    INFO("got (" << got.r << ", " << got.g << ", " << got.b << "), want (" << want.r << ", "
                 << want.g << ", " << want.b << ")");
    CHECK(glm::all(glm::lessThan(glm::abs(got - want), glm::vec3(2e-3f))));
}

const glm::vec3 kZoneCentre = worldOf(ZONE_X, ZONE_Y);

}  // namespace

TEST_CASE("outside every volume a point is lit by its map's global light", "[lighting]") {
    // Five hundred yards past the zone's edge.
    const glm::vec3 pos = kZoneCentre + glm::vec3(700.0f, 0.0f, 0.0f);
    requireColour(skyAt("light_blend_outside", pos), rgb(colourOf(GLOBAL_PARAMS)));
}

TEST_CASE("across a volume's falloff its light gives way linearly to the global light",
          "[lighting]") {
    // Halfway from the zone's inner radius to its outer one, on the side away
    // from the hamlet: half the zone, half the map's own light.
    const glm::vec3 pos = kZoneCentre - glm::vec3(0.0f, 150.0f, 0.0f);
    requireColour(skyAt("light_blend_falloff", pos),
                  glm::mix(rgb(colourOf(GLOBAL_PARAMS)), rgb(colourOf(ZONE_PARAMS)), 0.5f));
}

TEST_CASE("where two volumes hold a point whole, the one whose centre is nearer lights it",
          "[lighting]") {
    // At the hamlet's centre, well inside the zone's inner radius too.
    const glm::vec3 pos = worldOf(ZONE_X + HAMLET_OFFSET * LIGHT_COORD_UNITS_PER_YARD, ZONE_Y);
    requireColour(skyAt("light_blend_nested", pos), rgb(colourOf(HAMLET_PARAMS)));
}

TEST_CASE("a volume three yards across or less lights nothing", "[lighting]") {
    // At the speck's centre, inside the zone and away from the hamlet.
    const glm::vec3 pos = worldOf(ZONE_X + SPECK_OFFSET * LIGHT_COORD_UNITS_PER_YARD, ZONE_Y);
    requireColour(skyAt("light_blend_speck", pos), rgb(colourOf(ZONE_PARAMS)));
}
