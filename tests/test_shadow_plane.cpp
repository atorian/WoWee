// The plane a shadow receiver is compared in (shadow_plane.glsli).
//
// A receiver compares each PCF tap against its own surface's depth at that
// tap. What matters is that the slope it is handed predicts that depth: walk
// along the surface, and the map's depth there is the receiver's plus the
// slope times the step across the map. That has to hold for the maps the
// renderer builds - the sun's, Y-flipped for Vulkan, and the units' - for any
// surface the sun is not edge-on to, whichever way its normal points and
// however long it is, and it must not depend on anything but the surface and
// the map: the shimmer this replaced came from a slope the camera decided.

#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

// The shader's own function, run on the CPU.
namespace shadow_shader {
using glm::clamp;
using glm::dot;
using glm::mat3;
using glm::mat4;
using glm::vec2;
using glm::vec3;
using std::abs;
#include "../assets/shaders/shadow_plane.glsli"
}  // namespace shadow_shader

using Catch::Approx;
using shadow_shader::shadowPlaneSlope;

namespace {

/// How far a prediction may miss, in yards along the light: a centimetre, a
/// third of the hand's breadth the receivers keep as bias. Float positions a
/// few thousand yards from the origin cost a few millimetres of it.
constexpr float kMissYards = 0.01f;

/// A map as Renderer::computeLightSpaceMatrix builds the sun's: centred on a
/// point, looking along the rays, orthographic, Y flipped for Vulkan.
glm::mat4 sunMap(glm::vec3 centre, glm::vec3 rays, float halfExtent) {
    rays = glm::normalize(rays);
    glm::mat4 projection = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent,
                                      1.0f, halfExtent * 6.5f);
    projection[1][1] *= -1.0f;
    return projection * glm::lookAt(centre - rays * (halfExtent * 3.0f), centre,
                                    glm::vec3(0.0f, 0.0f, 1.0f));
}

/// The units' map: 40 yards across round the player, a depth range of 128.
glm::mat4 unitsMap(glm::vec3 centre, glm::vec3 rays) {
    rays = glm::normalize(rays);
    glm::mat4 projection = glm::ortho(-20.0f, 20.0f, -20.0f, 20.0f, 0.1f, 128.0f);
    projection[1][1] *= -1.0f;
    return projection * glm::lookAt(centre - rays * 64.0f, centre, glm::vec3(0.0f, 0.0f, 1.0f));
}

/// Where a world point lands in a map: uv and depth, as the receivers take them.
glm::vec3 inMap(const glm::mat4& map, glm::vec3 p) {
    const glm::vec4 clip = map * glm::vec4(p, 1.0f);
    return {clip.x * 0.5f + 0.5f, clip.y * 0.5f + 0.5f, clip.z};
}

/// Two directions lying in the plane of `n`.
void tangents(glm::vec3 n, glm::vec3& t, glm::vec3& b) {
    n = glm::normalize(n);
    const glm::vec3 other = std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    t = glm::normalize(glm::cross(n, other));
    b = glm::cross(n, t);
}

/// How far off the slope's prediction of the map depth is, a yard along the
/// surface each way from `p`, in yards along the light.
float worstMiss(const glm::mat4& map, float depthPerYard, glm::vec3 p, glm::vec3 n) {
    const glm::vec2 slope = shadowPlaneSlope(map, n);
    const glm::vec3 here = inMap(map, p);
    glm::vec3 t, b;
    tangents(n, t, b);
    float worst = 0.0f;
    for (const glm::vec3 step : {t, -t, b, -b, glm::normalize(t + b)}) {
        const glm::vec3 there = inMap(map, p + step);
        const float predicted = here.z + glm::dot(slope, glm::vec2(there) - glm::vec2(here));
        worst = std::max(worst, std::abs(predicted - there.z) / depthPerYard);
    }
    return worst;
}

float depthPerYard(const glm::mat4& map) {
    return glm::length(glm::vec3(map[0][2], map[1][2], map[2][2]));
}

}  // namespace

TEST_CASE("A receiver's plane predicts the sun map's depth along its surface", "[shadow_plane]") {
    // Dalaran's square and a low afternoon sun, as the client sees them.
    const glm::vec3 centre(5804.0f, 624.0f, 647.0f);
    const glm::vec3 rays(-0.45f, 0.35f, -0.55f);
    const glm::mat4 map = sunMap(centre, rays, 500.0f);
    const float perYard = depthPerYard(map);

    SECTION("flat ground") {
        CHECK(worstMiss(map, perYard, centre, glm::vec3(0, 0, 1)) < kMissYards);
    }
    SECTION("a wall facing the sun and one turned from it") {
        CHECK(worstMiss(map, perYard, centre + glm::vec3(3, 0, 2), glm::vec3(1, 0, 0)) < kMissYards);
        CHECK(worstMiss(map, perYard, centre + glm::vec3(0, -4, 2), glm::vec3(0, -1, 0)) < kMissYards);
    }
    SECTION("a leaf card tilted every which way") {
        for (const glm::vec3 n : {glm::vec3(0.3f, 0.8f, 0.5f), glm::vec3(-0.7f, 0.1f, 0.7f),
                                  glm::vec3(0.2f, -0.6f, -0.77f)}) {
            CHECK(worstMiss(map, perYard, centre + glm::vec3(-2, 1, 5), n) < kMissYards);
        }
    }
}

TEST_CASE("The units' map takes the same plane", "[shadow_plane]") {
    const glm::vec3 player(-9449.0f, 64.0f, 56.4f);
    const glm::mat4 map = unitsMap(player, glm::vec3(0.2f, -0.3f, -0.93f));
    const float perYard = depthPerYard(map);
    CHECK(worstMiss(map, perYard, player, glm::vec3(0, 0, 1)) < kMissYards);
    CHECK(worstMiss(map, perYard, player + glm::vec3(1, 1, 1), glm::vec3(0.6f, 0.0f, 0.8f)) < kMissYards);
}

TEST_CASE("Which way a normal points and how long it is do not change the plane", "[shadow_plane]") {
    // A face normal taken from position derivatives has whatever length and
    // side the triangle's winding and size give it.
    const glm::mat4 map = sunMap(glm::vec3(100.0f, 200.0f, 30.0f), glm::vec3(0.1f, 0.4f, -0.9f), 200.0f);
    const glm::vec3 n(0.25f, -0.4f, 0.88f);
    const glm::vec2 unit = shadowPlaneSlope(map, glm::normalize(n));
    for (const float scale : {1e-4f, 0.37f, 52.0f, -1.0f, -3e-3f}) {
        const glm::vec2 s = shadowPlaneSlope(map, n * scale);
        CHECK(s.x == Approx(unit.x).margin(1e-4));
        CHECK(s.y == Approx(unit.y).margin(1e-4));
    }
}

TEST_CASE("A surface edge-on to the sun gets a bounded slope, not a runaway", "[shadow_plane]") {
    // A wall the rays run along: its depth across the map is unbounded. The
    // taps must stay on the map, so the slope stops at the clamp - and is a
    // number, not infinity or NaN.
    const glm::vec3 rays = glm::normalize(glm::vec3(1.0f, 0.0f, -0.5f));
    const glm::mat4 map = sunMap(glm::vec3(0.0f), rays, 300.0f);
    const glm::vec3 alongRays = glm::normalize(glm::cross(rays, glm::vec3(0, 1, 0)));
    const glm::vec2 s = shadowPlaneSlope(map, glm::cross(alongRays, rays));
    CHECK(std::isfinite(s.x));
    CHECK(std::isfinite(s.y));
    CHECK(std::abs(s.x) <= 64.0f);
    CHECK(std::abs(s.y) <= 64.0f);
}
