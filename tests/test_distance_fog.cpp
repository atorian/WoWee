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
    const vec3 sun(0, 0, 1);
    const auto sky = [&](vec3 ray) {
        return atmosphereSkyColor(ray, sun, top, middle, band1, fringe);
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
