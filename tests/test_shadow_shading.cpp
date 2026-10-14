// The client's rule for how a surface takes a shadow, from the receiving
// shaders of WoW 3.3.5a: a shadow leaves 70% of the lit colour and fades out
// over the outer 30% of the map.
#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>

#include <cmath>

namespace {
using vec2 = glm::vec2;
using glm::clamp;
using glm::max;
using std::abs;
#include "../assets/shaders/shadow_shading.glsli"
}

TEST_CASE("a full shadow leaves seventy percent of the lit colour", "[shadow]") {
    CHECK(shadowLightFactor(1.0f) == Catch::Approx(1.0f));
    CHECK(shadowLightFactor(0.0f) == Catch::Approx(0.7f));
    CHECK(shadowLightFactor(0.5f) == Catch::Approx(0.85f));
}

TEST_CASE("a shadow map fades out over the outer part of its extent", "[shadow]") {
    // Whole up to 70% of the half extent, gone at 99%.
    CHECK(shadowMapEdgeFade(vec2(0.0f)) == Catch::Approx(1.0f));
    CHECK(shadowMapEdgeFade(vec2(0.7f, 0.0f)) == Catch::Approx(1.0f).margin(1e-5));
    CHECK(shadowMapEdgeFade(vec2(0.0f, -0.99f)) == Catch::Approx(0.0f).margin(1e-5));
    CHECK(shadowMapEdgeFade(vec2(0.845f, 0.2f)) == Catch::Approx(0.5f).margin(1e-3));
    CHECK(shadowMapEdgeFade(vec2(1.0f, 1.0f)) == 0.0f);
}
