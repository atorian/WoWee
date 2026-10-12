// From light volumes to the frame's LightingParams: which LightParams row a
// volume names for the weather, what a row says at an hour when some of its
// bands are missing, and how the volumes add up when one of them names a row
// that is not there.
#include <catch_amalgamated.hpp>

#include <array>

#include "rendering/light_params_eval.hpp"

using namespace wowee::rendering;

namespace {

LightingParams fallbackLight() {
    LightingParams p;
    p.fogStart = 300.0f;
    p.fogEnd = 1500.0f;
    p.fogDensity = 0.001f;
    p.cloudDensity = 0.3f;
    return p;
}

void setFloatBand(LightParamsProfile& profile, int channel, float value) {
    FloatBand& band = profile.floatBands[channel];
    band.numKeyframes = 1;
    band.times[0] = 0;
    band.values[0] = value;
}

LightingParams lit(float ambient, float fogEnd) {
    LightingParams p;
    p.ambientColor = glm::vec3(ambient);
    p.fogStart = fogEnd * 0.5f;
    p.fogEnd = fogEnd;
    p.directionalDir = glm::vec3(0, 0, -1);
    return p;
}

}  // namespace

TEST_CASE("a row with no fog distance band keeps the fallback's fog", "[light][fog]") {
    // Taking 1.0 for the missing band and dividing it by 36 put the fog's end
    // at 0.03 yards: everything past the camera fully fogged.
    const LightParamsProfile profile{};
    const LightingParams p = sampleLightParams(profile, 1440, fallbackLight());
    CHECK(p.fogEnd == 1500.0f);
    CHECK(p.fogStart == 300.0f);
}

TEST_CASE("a row's fog end is in thirty-sixths of a yard and its start a fraction of it",
          "[light][fog]") {
    LightParamsProfile profile;
    setFloatBand(profile, LightParamsProfile::FOG_END, 12000.0f);
    setFloatBand(profile, LightParamsProfile::FOG_START_SCALAR, 0.25f);
    const LightingParams p = sampleLightParams(profile, 1440, fallbackLight());
    CHECK(p.fogEnd == Catch::Approx(333.33f).epsilon(1e-4));
    CHECK(p.fogStart == Catch::Approx(83.33f).epsilon(1e-4));
}

TEST_CASE("a row with a fog end but no start fraction takes the fallback's fraction",
          "[light][fog]") {
    // The fallback starts its fog at a fifth of the way out.
    LightParamsProfile profile;
    setFloatBand(profile, LightParamsProfile::FOG_END, 12000.0f);
    const LightingParams p = sampleLightParams(profile, 1440, fallbackLight());
    CHECK(p.fogStart == Catch::Approx(66.67f).epsilon(1e-4));
}

TEST_CASE("the cloud curve is not read as fog density", "[light][fog]") {
    LightParamsProfile profile;
    setFloatBand(profile, LightParamsProfile::CLOUD_DENSITY, 4.0f);
    const LightingParams p = sampleLightParams(profile, 1440, fallbackLight());
    CHECK(p.cloudDensity == 4.0f);
    CHECK(p.fogDensity == 0.001f);
}

TEST_CASE("a row with no cloud band keeps the fallback's clouds", "[light]") {
    const LightingParams p = sampleLightParams(LightParamsProfile{}, 1440, fallbackLight());
    CHECK(p.cloudDensity == 0.3f);
}

TEST_CASE("a float band interpolates between keyframes and across midnight", "[light]") {
    // Keyframes at 06:00 and 18:00.
    FloatBand band;
    band.numKeyframes = 2;
    band.times[0] = 720;
    band.times[1] = 2160;
    band.values[0] = 100.0f;
    band.values[1] = 300.0f;
    CHECK(sampleFloatBand(band, 1440) == Catch::Approx(200.0f));  // noon
    CHECK(sampleFloatBand(band, 0) == Catch::Approx(200.0f));     // midnight, halfway back
    CHECK(sampleFloatBand(band, 2520) == Catch::Approx(250.0f));  // 21:00
    CHECK_FALSE(sampleFloatBand(FloatBand{}, 1440).has_value());
}

TEST_CASE("a volume naming a missing row does not dim the one that is there", "[light][fog]") {
    // Two volumes at half each, the second naming a row the file does not
    // have: what is left is the first volume's light, not half of it.
    const std::array<WeightedLightingParams, 1> parts{{{.params = lit(0.8f, 400.0f), .weight = 0.5f}}};
    const auto p = blendLightingParams(parts);
    REQUIRE(p.has_value());
    CHECK(p->ambientColor.r == Catch::Approx(0.8f));
    CHECK(p->fogEnd == Catch::Approx(400.0f));
    CHECK(p->fogStart == Catch::Approx(200.0f));
}

TEST_CASE("volumes blend by their share of the weight that is there", "[light][fog]") {
    const std::array<WeightedLightingParams, 2> parts{{
        {.params = lit(0.2f, 100.0f), .weight = 0.1f},
        {.params = lit(0.6f, 500.0f), .weight = 0.3f},
    }};
    const auto p = blendLightingParams(parts);
    REQUIRE(p.has_value());
    CHECK(p->ambientColor.r == Catch::Approx(0.5f));
    CHECK(p->fogEnd == Catch::Approx(400.0f));
}

TEST_CASE("no weight is no blend rather than black with fog at zero", "[light][fog]") {
    CHECK_FALSE(blendLightingParams({}).has_value());
    const std::array<WeightedLightingParams, 1> weightless{{{.params = lit(0.8f, 400.0f), .weight = 0.0f}}};
    CHECK_FALSE(blendLightingParams(weightless).has_value());
}

TEST_CASE("the weather and the camera choose the volume's column", "[light]") {
    LightVolume v;
    v.lightParamsId = 1;
    v.lightParamsIdUnderwater = 2;
    v.lightParamsIdStorm = 3;
    v.lightParamsIdStormUnderwater = 4;
    CHECK(selectLightParamsId(v, false, false) == 1);
    CHECK(selectLightParamsId(v, false, true) == 2);
    CHECK(selectLightParamsId(v, true, false) == 3);
    CHECK(selectLightParamsId(v, true, true) == 4);
}

TEST_CASE("an empty column falls back to the nearest case", "[light]") {
    LightVolume v;
    v.lightParamsId = 1;
    v.lightParamsIdUnderwater = 2;
    v.lightParamsIdStorm = 3;
    // No storm-under-water row: under water still wins over the storm.
    CHECK(selectLightParamsId(v, true, true) == 2);

    LightVolume dry;
    dry.lightParamsId = 1;
    dry.lightParamsIdStorm = 3;
    // No water rows at all: the weather decides.
    CHECK(selectLightParamsId(dry, false, true) == 1);
    CHECK(selectLightParamsId(dry, true, true) == 3);
}
