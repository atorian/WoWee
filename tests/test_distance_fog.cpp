#include <catch_amalgamated.hpp>
#include "rendering/lighting_manager.hpp"

// Run the production shader math, rather than a separate C++ approximation.
namespace fog_math {
using vec3 = glm::vec3;
using glm::clamp;
using glm::dot;
using glm::exp;
using glm::max;
using glm::mix;
using glm::pow;
using glm::smoothstep;
using glm::sqrt;
#include "../assets/shaders/sky_atmosphere.glsli"
#include "../assets/shaders/sunlit_air.glsli"
}

TEST_CASE("zone light blending has no fallback sky or extra fog distance", "[light][fog]") {
    const auto p = wowee::rendering::LightingParams::blendAccumulator();
    for (const auto& color : {p.ambientColor, p.diffuseColor, p.directionalDir,
                             p.fogColor, p.skyTopColor, p.skyMiddleColor,
                             p.skyBand1Color, p.skyBand2Color}) {
        CHECK(color == glm::vec3(0));
    }
    for (float value : {p.fogStart, p.fogEnd, p.fogDensity, p.cloudDensity, p.horizonGlow}) {
        CHECK(value == 0.0f);
    }
    // The ordinary fallback remains usable; only weighted sums start at zero.
    const wowee::rendering::LightingParams fallback;
    CHECK(fallback.fogEnd == 1000.0f);
    CHECK(fallback.skyBand2Color.r == 1.0f);
}

TEST_CASE("sky below the horizon and fully fogged terrain use the same authored colour", "[light][fog]") {
    using namespace fog_math;
    const vec3 top(.2f, .15f, .3f), middle(.7f, .6f, .8f);
    const vec3 band1(.35f, .2f, .4f), band2(.25f, .1f, .3f);
    const vec3 smog(.5f, .4f, .1f), fog(.1f, .08f, .2f);
    const auto sky = [&](vec3 ray) {
        return atmosphereSkyColor(ray, top, middle, band1, band2, smog, fog);
    };
    const vec3 distant = mix(fog, vec3(1.0f), distanceFogVisibility(2000, 100, 800));
    CHECK(sky(glm::normalize(vec3(1, 0, -.2f))) == distant);
    CHECK(sky(vec3(0, 0, -1)) == distant);
    CHECK(sky(vec3(0, 0, 1)) == top);
    // A green smog channel has its own place close to the horizon; it must
    // not be skipped on the way from the upper sky to the fog below it.
    const vec3 nearHorizon = atmosphereSkyColor(glm::normalize(vec3(1, 0, .02f)),
        vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0),
        vec3(0, 1, 0), vec3(0, 0, 1));
    CHECK(nearHorizon.g > nearHorizon.r);
    CHECK(nearHorizon.g > nearHorizon.b);
}

TEST_CASE("the sky dome adds no azimuth-dependent colour to its bands", "[light][fog]") {
    using namespace fog_math;
    const vec3 top(.15f, .3f, .25f), middle(.17f, .37f, .29f);
    const vec3 band1(.25f, .4f, .33f), band2(.33f, .58f, .53f);
    const vec3 smog(.3f), fog(.2f, .15f, .3f);
    const vec3 toward = atmosphereSkyColor(glm::normalize(vec3(0, .6f, .8f)),
        top, middle, band1, band2, smog, fog);
    const vec3 opposite = atmosphereSkyColor(glm::normalize(vec3(0, -.6f, .8f)),
        top, middle, band1, band2, smog, fog);
    CHECK(glm::length(toward - opposite) < 1e-6f);
}

TEST_CASE("sunlit air glows toward the sun in the zone's halo, and is lit by the sun elsewhere",
          "[light][fog]") {
    // Tirisfal at a quarter past four: the sun casts a cool white, and its
    // halo is a dim green. The air toward the sun was the cool white too, a
    // white blot over the green sun the light tables draw.
    using namespace fog_math;
    const vec3 sunLight(.64f, .70f, .86f);
    const vec3 halo(.22f, .42f, .33f);

    // Looking into the sun through the air: green, the halo's hue.
    const vec3 intoSun = sunlitAirLight(sunLight, halo, 1.0f);
    CHECK(intoSun.g > intoSun.r);
    CHECK(intoSun.g > intoSun.b);
    CHECK(intoSun.r / intoSun.g == Catch::Approx(halo.r / halo.g).margin(0.05));
    // As bright as the halo is: a halo twice as bright, a glow twice as bright.
    const vec3 brighter = sunlitAirLight(sunLight, halo * 2.0f, 1.0f);
    CHECK(brighter.g > 1.8f * intoSun.g);

    // With the sun behind the eye the forward glow is gone, and the air is lit
    // by the light the sun casts - blue the brightest, as in that light.
    const vec3 sunBehind = sunlitAirLight(sunLight, halo, -1.0f);
    CHECK(sunBehind.b > sunBehind.g);
    CHECK(sunBehind.b > sunBehind.r);
}

TEST_CASE("fog leaves nearby surfaces clear and has finite degenerate ranges", "[light][fog]") {
    using namespace fog_math;
    CHECK(distanceFogVisibility(50, 100, 800) == 1.0f);
    CHECK(distanceFogVisibility(100, 100, 800) == 1.0f);
    CHECK(distanceFogVisibility(450, 100, 800) == Catch::Approx(.5f));
    CHECK(distanceFogVisibility(800, 100, 800) == 0.0f);
    CHECK(distanceFogVisibility(8000, 100, 800) == 0.0f);
    CHECK(distanceFogVisibility(100, 100, 100) == 0.0f);
    const auto p = glm::vec3(5800, 600, 650);
    const auto d = glm::vec3(1000, 300, -200);
    const auto ray = glm::normalize(d);
    const auto translated = glm::normalize((p + d) - p);
    CHECK(ray == translated);
}
