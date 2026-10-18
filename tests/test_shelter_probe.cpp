// What the shelter map says of the camera's place (shelter_probe.glsli, which
// ShelterMap's probe runs on the GPU): whether the sky is over it, and how
// much of the sky its surroundings leave it - the open field, a porch, a
// street between high walls and a wide courtyard.
#include <catch_amalgamated.hpp>

#include "rendering/glsl_as_cpp.hpp"

#include <functional>

namespace wowee::rendering::glsl {
namespace {
/// The world the test stands the camera in: the highest surface over a point.
std::function<float(vec2)> gWorld;
float shelterTop(vec2 xy);
#include "../assets/shaders/shelter_probe.glsli"
float shelterTop(vec2 xy) { return gWorld(xy); }
}  // namespace
}  // namespace wowee::rendering::glsl

using wowee::rendering::glsl::gWorld;
using wowee::rendering::glsl::kNoShelter;
using wowee::rendering::glsl::shelterExposure;
using wowee::rendering::glsl::shelterSkyView;

namespace {

constexpr float kTexel = 0.25f;  // ShelterMap's
const glm::vec3 kEye(100.0f, 200.0f, 51.7f);  // standing on ground at 50

/// Ground at 50, and walls `height` over the eye from `halfWidth` yards
/// either side of it along x - a street running along y.
float street(glm::vec2 xy, float halfWidth, float height) {
    return std::abs(xy.x - kEye.x) >= halfWidth ? kEye.z + height : 50.0f;
}

}  // namespace

TEST_CASE("In an open field the camera is under the sky and sees all of it", "[weather][shelter]") {
    gWorld = [](glm::vec2) { return 50.0f; };
    CHECK(shelterExposure(kEye, kTexel) == Catch::Approx(1.0f));
    CHECK(shelterSkyView(kEye) == Catch::Approx(1.0f));
}

TEST_CASE("Where nothing was drawn the sky is open", "[weather][shelter]") {
    gWorld = [](glm::vec2) { return kNoShelter; };
    CHECK(shelterExposure(kEye, kTexel) == Catch::Approx(1.0f));
    CHECK(shelterSkyView(kEye) == Catch::Approx(1.0f));
}

TEST_CASE("Under a porch roof the camera is out of the sky", "[weather][shelter]") {
    gWorld = [](glm::vec2 xy) {
        return glm::length(xy - glm::vec2(kEye.x, kEye.y)) < 4.0f ? kEye.z + 2.0f : 50.0f;
    };
    CHECK(shelterExposure(kEye, kTexel) == Catch::Approx(0.0f));
}

TEST_CASE("At the edge of a roof the camera is partly under it", "[weather][shelter]") {
    gWorld = [](glm::vec2 xy) { return xy.x < kEye.x ? kEye.z + 2.0f : 50.0f; };
    const float e = shelterExposure(kEye, kTexel);
    CHECK(e > 0.2f);
    CHECK(e < 0.8f);
}

TEST_CASE("A street between high walls leaves less of the sky than a courtyard, and a field all of it",
          "[weather][shelter]") {
    gWorld = [](glm::vec2 xy) { return street(xy, 3.0f, 8.0f); };
    const float narrow = shelterSkyView(kEye);
    CHECK(shelterExposure(kEye, kTexel) == Catch::Approx(1.0f));  // nothing over the street itself

    gWorld = [](glm::vec2 xy) { return street(xy, 20.0f, 8.0f); };
    const float wide = shelterSkyView(kEye);

    CHECK(narrow < 0.7f);
    CHECK(narrow < wide);
    CHECK(wide < 1.0f);
}

TEST_CASE("Walls lower than the eye hide none of the sky", "[weather][shelter]") {
    gWorld = [](glm::vec2 xy) { return street(xy, 3.0f, -0.5f); };
    CHECK(shelterSkyView(kEye) == Catch::Approx(1.0f));
}
