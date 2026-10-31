// Which LightParams channel lights what.
//
// LightIntBand.dbc carries eighteen colour channels per LightParams row and
// LightFloatBand.dbc six numbers, none of them named in the file. Each was
// once read into the wrong field: channel 0 taken as the ambient made
// Stormwind orange, channel 6 taken as the fog greyed every horizon, a colour
// read low byte first turned Teldrassil's violet magenta, channel 2 taken as
// cloud density put every zone under overcast, and the fog distance used in
// raw thirty-sixths of a yard pushed the fog six times past the far clip.
//
// The mapping in the comments of lighting_manager.hpp was settled against the
// shipped files and WoW.exe. Here a single global light is given a different
// colour on every channel, and the lit scene is read back from
// LightingManager to see which colour went where.
#include <catch_amalgamated.hpp>

#include "rendering/light_coords.hpp"
#include "rendering/lighting_manager.hpp"

#include "lighting_tables.hpp"
#include "test_support.hpp"

#include <bit>
#include <cmath>
#include <vector>

using wowee::rendering::LightingManager;
using wowee::rendering::LightingParams;
using wowee::rendering::LIGHT_COORD_UNITS_PER_YARD;

namespace {

/// A colour of its own for each channel, 0x00RRGGBB: eighteen hues twenty
/// degrees apart, so no two channels share one and red is never blue.
uint32_t channelColour(uint32_t channel) {
    const float h = static_cast<float>(channel) * 20.0f / 60.0f;  // sector, 0..6
    const float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
    glm::vec3 c;
    switch (static_cast<int>(h) % 6) {
        case 0: c = {1, x, 0}; break;
        case 1: c = {x, 1, 0}; break;
        case 2: c = {0, 1, x}; break;
        case 3: c = {0, x, 1}; break;
        case 4: c = {x, 0, 1}; break;
        default: c = {1, 0, x}; break;
    }
    c = c * 0.8f + 0.1f;  // keep every component off zero
    const auto byte = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f); };
    return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

glm::vec3 rgb(uint32_t packed) {
    return glm::vec3((packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff) / 255.0f;
}

std::vector<uint8_t> lightDbc() {
    // The map's global light, at the origin with a yard of extent.
    std::vector<uint32_t> row(15, 0);
    row[0] = 1;
    row[5] = std::bit_cast<uint32_t>(1.0f * LIGHT_COORD_UNITS_PER_YARD);
    row[6] = std::bit_cast<uint32_t>(1.0f * LIGHT_COORD_UNITS_PER_YARD);
    row[7] = row[8] = row[9] = row[10] = 1;  // LightParams 1, in every weather
    return wowee::test::wdbcFile(15, {row}, std::string(1, '\0'));
}

std::vector<uint8_t> lightParamsDbc() {
    return wowee::test::wdbcFile(9, {{1, 0, 0, 0, 0, 0, 0, 0, 0}}, std::string(1, '\0'));
}

/// One band per channel of LightParams 1, the same at every hour: the id, two
/// keyframes, the times, then the values.
std::vector<uint8_t> intBandDbc() {
    std::vector<std::vector<uint32_t>> rows;
    for (uint32_t channel = 0; channel < 18; ++channel) {
        std::vector<uint32_t> row(34, 0);
        row[0] = channel + 1;
        row[1] = 2;
        row[2] = 0;
        row[3] = 1440;
        row[18] = row[19] = channelColour(channel);
        rows.push_back(row);
    }
    return wowee::test::wdbcFile(34, rows, std::string(1, '\0'));
}

/// The six float channels: fog end 12000 (Tirisfal's, in thirty-sixths of a
/// yard), fog start half of it, the celestial switch at 1, clouds at 0.25.
std::vector<uint8_t> floatBandDbc() {
    const float values[6] = {12000.0f, 0.5f, 1.0f, 0.25f, 0.0f, 0.0f};
    std::vector<std::vector<uint32_t>> rows;
    for (uint32_t channel = 0; channel < 6; ++channel) {
        std::vector<uint32_t> row(34, 0);
        row[0] = channel + 1;
        row[1] = 2;
        row[2] = 0;
        row[3] = 1440;
        row[18] = row[19] = std::bit_cast<uint32_t>(values[channel]);
        rows.push_back(row);
    }
    return wowee::test::wdbcFile(34, rows, std::string(1, '\0'));
}

/// What a point under that light is lit with at noon, once the smoothing has
/// settled.
LightingParams litAtNoon() {
    LightingManager lighting;
    const wowee::test::LightingTables data({
        {"Light.dbc", lightDbc()},
        {"LightParams.dbc", lightParamsDbc()},
        {"LightIntBand.dbc", intBandDbc()},
        {"LightFloatBand.dbc", floatBandDbc()},
    });
    lighting.initialize(data.tables);
    for (int i = 0; i < 11; ++i) lighting.update(0.25f, glm::vec3(0.0f), 0, 0, 12.0f, 0.0f);
    return lighting.getLightingParams();
}

void requireColour(const glm::vec3& got, uint32_t channel) {
    const glm::vec3 want = rgb(channelColour(channel));
    INFO("channel " << channel << ": got (" << got.r << ", " << got.g << ", " << got.b
                    << "), want (" << want.r << ", " << want.g << ", " << want.b << ")");
    CHECK(glm::all(glm::lessThan(glm::abs(got - want), glm::vec3(2e-3f))));
}

/// The channel whose colour has the hue `got` has, for the two the renderer
/// scales after reading: the sun's by its strength, the ambient by its own.
uint32_t nearestChannel(const glm::vec3& got) {
    uint32_t best = 0;
    float bestCos = -2.0f;
    for (uint32_t channel = 0; channel < 18; ++channel) {
        const float c = glm::dot(glm::normalize(got), glm::normalize(rgb(channelColour(channel))));
        if (c > bestCos) { bestCos = c; best = channel; }
    }
    return best;
}

}  // namespace

TEST_CASE("channel 0 is the sunlight and channel 1 the ambient", "[light]") {
    const LightingParams lit = litAtNoon();
    CHECK(nearestChannel(lit.diffuseColor) == 0);
    CHECK(nearestChannel(lit.ambientColor) == 1);
}

TEST_CASE("channels 2 to 5 are the sky from the top down, 6 its smog and 7 the fog", "[light]") {
    const LightingParams lit = litAtNoon();
    requireColour(lit.skyTopColor, 2);
    requireColour(lit.skyMiddleColor, 3);
    requireColour(lit.skyBand1Color, 4);
    requireColour(lit.skyBand2Color, 5);
    requireColour(lit.skySmogColor, 6);
    requireColour(lit.fogColor, 7);
}

TEST_CASE("the fog ends where the band says, in yards", "[light]") {
    const LightingParams lit = litAtNoon();
    // 12000 / 36 = 333.3 yards, inside the far clip; used raw it is 12000.
    CHECK(lit.fogEnd == Catch::Approx(333.33f).epsilon(1e-3));
    CHECK(lit.fogStart == Catch::Approx(166.67f).epsilon(1e-3));
}

TEST_CASE("cloud density is channel 3, not the switch in channel 2", "[light]") {
    CHECK(litAtNoon().cloudDensity == Catch::Approx(0.25f));
}
