// A zone is lit by the light volumes it stands in, and by nothing that knows
// its name.
//
// WoW.exe 3.3.5a gives every zone its look through Light.dbc: the volumes over
// it name LightParams rows, whose LightIntBand curves are the sun, the ambient,
// the fog and the sky at each hour of the world clock. Tirisfal's green sky and
// purple haze, Duskwood's blue gloom are those curves.
//
// The renderer had a table of its own for zones 10, 85 and 130 that capped
// their light, replaced their fog and sky with picked colours, and held
// Duskwood's sky clock at ten at night. Tirisfal came out grey where its curves
// are green, and Duskwood never saw its noon key.
//
// Each case is one light volume made up here, read by LightingManager as it
// reads the client's files, and stood in under different zone ids.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/sky_params_from_lighting.hpp"

#include "test_support.hpp"

#include <bit>
#include <chrono>
#include <thread>
#include <vector>

using wowee::rendering::LightingManager;
using wowee::rendering::LightingParams;

namespace {

constexpr uint32_t PARAMS_ID = 1;
constexpr float DBC_X = 1000.0f;
constexpr float DBC_Y = 1000.0f;

std::vector<uint8_t> lightDbc() {
    std::vector<uint32_t> row(15, 0);
    row[0] = 1;
    row[1] = 0;
    row[2] = std::bit_cast<uint32_t>(DBC_X);
    row[3] = std::bit_cast<uint32_t>(0.0f);
    row[4] = std::bit_cast<uint32_t>(DBC_Y);
    row[5] = std::bit_cast<uint32_t>(100.0f * wowee::rendering::LIGHT_COORD_UNITS_PER_YARD);
    row[6] = std::bit_cast<uint32_t>(200.0f * wowee::rendering::LIGHT_COORD_UNITS_PER_YARD);
    // Clear, clear underwater, storm, storm underwater.
    row[7] = row[8] = row[9] = row[10] = PARAMS_ID;
    return wowee::test::wdbcFile(15, {row}, std::string(1, '\0'));
}

std::vector<uint8_t> lightParamsDbc() {
    return wowee::test::wdbcFile(9, {{PARAMS_ID, 0, 0, 0, 0, 0, 0, 0, 0}},
                                 std::string(1, '\0'));
}

/// Tirisfal's noon, as the client's LightIntBand has it: the light the sun
/// casts, a cool pinkish white; the sun's disc, a pale green; and the glow of
/// the sky around it, a darker green.
constexpr uint32_t NOON_SUN_LIGHT = 0xd0afca;  // channel 0
constexpr uint32_t NOON_SUN_DISC = 0xc3eac3;   // channel 9
constexpr uint32_t NOON_SUN_HALO = 0x386b54;   // channel 10

uint32_t noonColour(uint32_t channel) {
    switch (channel) {
        case 0: return NOON_SUN_LIGHT;
        case 9: return NOON_SUN_DISC;
        case 10: return NOON_SUN_HALO;
        default: return 0x30a060 + channel;
    }
}

glm::vec3 rgb(uint32_t packed) {
    return glm::vec3((packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff) / 255.0f;
}

/// Every colour channel of the row with two keys, as Tirisfal's and Duskwood's
/// rows have: a dark blue midnight and a bright noon.
std::vector<uint8_t> lightIntBandDbc() {
    std::vector<std::vector<uint32_t>> rows;
    for (uint32_t channel = 0; channel < 18; ++channel) {
        std::vector<uint32_t> row(34, 0);
        row[0] = (PARAMS_ID - 1) * 18 + channel + 1;
        row[1] = 2;
        row[2] = 0;       // midnight
        row[3] = 1440;    // noon, in half-minutes
        row[18] = 0x101830;
        row[19] = noonColour(channel);
        rows.push_back(row);
    }
    return wowee::test::wdbcFile(34, rows, std::string(1, '\0'));
}

/// What a player standing in the volume under `zoneId` at `hour` is lit by.
LightingParams lightIn(const char* rootName, uint32_t zoneId, float hour) {
    wowee::pipeline::AssetManager assets;
    LightingManager lighting;
    const auto root = wowee::test::makeDataRoot(rootName, {
        {"dbfilesclient\\light.dbc", lightDbc()},
        {"dbfilesclient\\lightparams.dbc", lightParamsDbc()},
        {"dbfilesclient\\lightintband.dbc", lightIntBandDbc()},
    });
    REQUIRE(assets.initialize(root.string()));
    REQUIRE(lighting.initialize(&assets));

    // Long enough for the light's smoothing, which runs on real time, to
    // settle on where the player stands.
    const glm::vec3 pos = wowee::rendering::lightPositionToWorld(DBC_X, DBC_Y, 0.0f);
    for (int i = 0; i < 8; ++i) {
        lighting.update(pos, 0, zoneId, hour);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    lighting.update(pos, 0, zoneId, hour);
    return lighting.getLightingParams();
}

void requireSameLight(const LightingParams& a, const LightingParams& b) {
    const auto same = [](const glm::vec3& x, const glm::vec3& y) {
        return glm::all(glm::lessThan(glm::abs(x - y), glm::vec3(1e-3f)));
    };
    CHECK(same(a.ambientColor, b.ambientColor));
    CHECK(same(a.diffuseColor, b.diffuseColor));
    CHECK(same(a.fogColor, b.fogColor));
    CHECK(same(a.skyTopColor, b.skyTopColor));
    CHECK(same(a.skyMiddleColor, b.skyMiddleColor));
    CHECK(same(a.skyBand1Color, b.skyBand1Color));
    CHECK(same(a.skyBand2Color, b.skyBand2Color));
    CHECK(same(a.sunColor, b.sunColor));
    CHECK(same(a.sunHaloColor, b.sunHaloColor));
    CHECK(a.fogStart == Catch::Approx(b.fogStart).margin(1e-2));
    CHECK(a.fogEnd == Catch::Approx(b.fogEnd).margin(1e-2));
}

}  // namespace

TEST_CASE("Tirisfal, Silverpine and Duskwood at noon are lit by their light volume", "[light]") {
    // Elwynn's zone id under the same volume is the reference: nothing there
    // but the volume's own curves.
    const LightingParams reference = lightIn("wowee_zone_light_elwynn", 12, 12.0f);

    SECTION("Tirisfal Glades") {
        requireSameLight(lightIn("wowee_zone_light_tirisfal", 85, 12.0f), reference);
    }
    SECTION("Silverpine Forest") {
        requireSameLight(lightIn("wowee_zone_light_silverpine", 130, 12.0f), reference);
    }
    SECTION("Duskwood") {
        requireSameLight(lightIn("wowee_zone_light_duskwood", 10, 12.0f), reference);
    }
}

TEST_CASE("The sky is handed the zone's sun colour, not the light it casts", "[light]") {
    // The disc and the glow around it were drawn from the cast light mixed
    // with fixed warm colours, so Tirisfal's green sun came out warm white.
    // WoW.exe tints both its disc and its glare with channel 9.
    const LightingParams lit = lightIn("wowee_zone_light_sun", 85, 12.0f);
    const wowee::rendering::SkyParams sky =
        wowee::rendering::skyParamsFromLighting(12.0f, 12.0f, 0.0f, &lit, false);

    const auto matches = [](const glm::vec3& x, const glm::vec3& y) {
        return glm::all(glm::lessThan(glm::abs(x - y), glm::vec3(2e-3f)));
    };
    CHECK(matches(sky.sunColor, rgb(NOON_SUN_DISC)));
    CHECK_FALSE(matches(sky.sunColor, lit.diffuseColor));
    // Channel 10, the zone's tint round the sun, is the air's forward glow.
    CHECK(matches(lit.sunHaloColor, rgb(NOON_SUN_HALO)));
}
