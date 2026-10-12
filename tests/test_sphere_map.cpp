// The sphere environment map M2 batches with texture coordinate set -1 sample.
// Runs the shader's own function. Expected values follow from GL_SPHERE_MAP: a
// normal tilted by an angle t from the line of sight lands sin(t)/2 from the
// centre of the map, towards the tilt.
#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

namespace sphere_map {
using glm::vec2;
using glm::vec3;
using glm::length;
using glm::max;
using glm::normalize;
using glm::reflect;
#include "../assets/shaders/sphere_map.glsli"
}

using Catch::Matchers::WithinAbs;

namespace {
// The camera looks down -Z in view space, so a vertex straight ahead of it
// sits on the negative Z axis.
const glm::vec3 kAhead(0.0f, 0.0f, -1.0f);
// cos(30 degrees); sin(30 degrees) is 0.5.
constexpr float kCos30 = 0.8660254f;
}

TEST_CASE("a surface facing the camera samples the centre of the sphere map",
          "[spheremap]") {
    const glm::vec2 uv = sphere_map::sphereMapUV(kAhead, glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK_THAT(uv.x, WithinAbs(0.5, 1e-5));
    CHECK_THAT(uv.y, WithinAbs(0.5, 1e-5));
}

TEST_CASE("a surface tilted 30 degrees from the line of sight lands a quarter "
          "of the way out, towards its tilt", "[spheremap]") {
    SECTION("tilted towards +x") {
        const glm::vec2 uv =
            sphere_map::sphereMapUV(kAhead, glm::vec3(0.5f, 0.0f, kCos30));
        CHECK_THAT(uv.x, WithinAbs(0.75, 1e-5));
        CHECK_THAT(uv.y, WithinAbs(0.5, 1e-5));
    }
    SECTION("tilted towards -y") {
        const glm::vec2 uv =
            sphere_map::sphereMapUV(kAhead, glm::vec3(0.0f, -0.5f, kCos30));
        CHECK_THAT(uv.x, WithinAbs(0.5, 1e-5));
        CHECK_THAT(uv.y, WithinAbs(0.25, 1e-5));
    }
}

TEST_CASE("a nearly camera-facing surface stays near the centre, not the rim",
          "[spheremap]") {
    // One degree of tilt: sin(1 degree) / 2 = 0.0087262 from the centre.
    const glm::vec3 normal(0.0174524f, 0.0f, 0.9998477f);
    const glm::vec2 uv = sphere_map::sphereMapUV(kAhead, normal);
    CHECK_THAT(uv.x, WithinAbs(0.5087262, 1e-5));
    CHECK_THAT(uv.y, WithinAbs(0.5, 1e-5));
}

TEST_CASE("the distance from the camera to the vertex does not move the sample",
          "[spheremap]") {
    // The same direction as kAhead, ten yards out.
    const glm::vec2 uv = sphere_map::sphereMapUV(glm::vec3(0.0f, 0.0f, -10.0f),
                                                 glm::vec3(0.5f, 0.0f, kCos30));
    CHECK_THAT(uv.x, WithinAbs(0.75, 1e-5));
    CHECK_THAT(uv.y, WithinAbs(0.5, 1e-5));
}
