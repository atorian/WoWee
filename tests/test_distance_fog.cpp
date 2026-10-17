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

TEST_CASE("distant haze uses the horizon and nadir instead of the middle sky", "[light][fog]") {
    using namespace fog_math;
    const vec3 top(.2f, .15f, .3f), middle(.7f, .6f, .8f);
    const vec3 band1(.35f, .2f, .4f), fringe(.25f, .1f, .3f);
    const vec3 sun(0, 0, 1), halo(1.0f, .78f, .54f);
    const auto sky = [&](vec3 ray) {
        return atmosphereSkyColor(ray, sun, top, middle, band1, fringe, halo);
    };
    // No sun disk on these rays. Existing horizon haze is shared with skybox.
    const vec3 horizon = sky(vec3(1, 0, 0));
    CHECK(horizon.r < middle.r);
    CHECK(horizon.g < middle.g);
    const vec3 below = sky(glm::normalize(vec3(1, 0, -.4f)));
    CHECK(below.r < horizon.r);
    CHECK(below.g < horizon.g);
    CHECK(sky(vec3(0, 0, -1)).r < horizon.r);

    const vec3 authored(.7f, .75f, .8f);
    // Ships far beyond the fog end converge on the same directional background.
    CHECK(distanceFogTint(authored, below, 2000, 100, 800, .7f) == below);
    CHECK(distanceFogTint(authored, below, 2000, 100, 800, 0) == authored);
    CHECK(distanceFogTint(authored, below, 800, 100, 800, 1) == below);
    const vec3 atEnd = distanceFogTint(authored, below, 800, 100, 800, .7f);
    const vec3 farther = distanceFogTint(authored, below, 1100, 100, 800, .7f);
    CHECK(atEnd == below);
    CHECK(farther == below);
}

TEST_CASE("the sky around the sun glows in the zone's halo colour", "[light][fog]") {
    // It glowed a fixed warm white and orange, so Tirisfal's green halo
    // (LightIntBand channel 10) never showed.
    using namespace fog_math;
    const vec3 top(.15f, .3f, .25f), middle(.17f, .37f, .29f);
    const vec3 band1(.25f, .4f, .33f), fringe(.33f, .58f, .53f);
    const vec3 sun = glm::normalize(vec3(0, .6f, .8f));
    const vec3 tirisfalHalo(.22f, .42f, .33f);

    const vec3 glow = atmosphereSkyColor(sun, sun, top, middle, band1, fringe, tirisfalHalo) -
                      atmosphereSkyColor(sun, sun, top, middle, band1, fringe, vec3(0.0f));
    CHECK(glow.g > 0.05f);
    // The glow is the halo, scaled: the same hue, no warm cast of its own.
    CHECK(glow.r / glow.g == Catch::Approx(tirisfalHalo.r / tirisfalHalo.g).epsilon(1e-3));
    CHECK(glow.b / glow.g == Catch::Approx(tirisfalHalo.b / tirisfalHalo.g).epsilon(1e-3));

    // Facing away from the sun there is none.
    const vec3 away = glm::normalize(vec3(0, -1, .2f));
    const vec3 none = atmosphereSkyColor(away, sun, top, middle, band1, fringe, tirisfalHalo) -
                      atmosphereSkyColor(away, sun, top, middle, band1, fringe, vec3(0.0f));
    CHECK(glm::length(none) < 1e-6f);
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

TEST_CASE("actual authored sky absorbs distant silhouettes before geometry clipping", "[light][fog]") {
    using namespace fog_math;
    const vec3 purpleSky(.18f, .07f, .31f), yellowZone(.9f, .7f, .15f);
    const vec3 brightShip(1.0f);
    const auto distant = [&](float distance) {
        return mix(distanceFogTint(yellowZone, purpleSky, distance, 100, 800, .7f),
                   brightShip, distanceFogVisibility(distance, 100, 800));
    };
    CHECK(distant(50) == brightShip);
    CHECK(distant(800) == purpleSky);
    CHECK(distant(1400) == purpleSky);
    CHECK(worldGeometryVisibility(2000, 2400) == 1.0f);
    CHECK(worldGeometryVisibility(2400, 2400) == 0.0f);
    CHECK(worldGeometryVisibility(3000, 2400) == 0.0f);
    CHECK(worldGeometryVisibility(2220, 2400) == Catch::Approx(.5f));
    CHECK(worldGeometryVisibility(4000, 0) == 1.0f);
}
