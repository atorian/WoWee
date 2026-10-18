// The sun and moon discs are round on screen wherever they stand in the sky.
//
// Each case looks straight at a body through a square perspective view, as the
// celestial vertex shader does (view translation removed), projects the
// body's quad and measures it on screen. The quad used to lie in the world's
// horizontal plane, so a sun at 37 degrees came out six tenths as tall as it
// was wide.
#include <catch_amalgamated.hpp>

#include "rendering/sky_billboard.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using wowee::rendering::skyBillboard;

namespace {

/// The quad's width and height on screen, in normalized device units, seen
/// by an eye at the origin looking straight at it.
glm::vec2 screenExtent(const glm::vec3& bodyPosition, float size) {
    const glm::vec3 dir = glm::normalize(bodyPosition);
    const glm::vec3 up =
        std::abs(dir.z) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), dir, up);
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 2000.0f);
    const glm::mat4 model = skyBillboard(bodyPosition, size);

    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const glm::vec2 corner : {glm::vec2(-0.5f, 0.5f), glm::vec2(0.5f, 0.5f),
                                   glm::vec2(0.5f, -0.5f), glm::vec2(-0.5f, -0.5f)}) {
        const glm::vec4 clip = projection * view * model * glm::vec4(corner, 0.0f, 1.0f);
        REQUIRE(clip.w > 0.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        lo = glm::min(lo, ndc);
        hi = glm::max(hi, ndc);
    }
    return hi - lo;
}

glm::vec3 atElevation(float degrees, float azimuthDegrees, float distance) {
    const float e = glm::radians(degrees), a = glm::radians(azimuthDegrees);
    return distance * glm::vec3(std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e));
}

}  // namespace

TEST_CASE("A sky body's disc is as tall as it is wide at any height", "[sky]") {
    // The client's afternoon sun stands about 37 degrees up; a rising sun at
    // 3; the moons wherever their path takes them, up to overhead.
    for (const float elevation : {3.0f, 20.0f, 37.0f, 60.0f, 85.0f, 90.0f}) {
        for (const float azimuth : {0.0f, 135.0f, 225.0f}) {
            CAPTURE(elevation, azimuth);
            const glm::vec2 extent = screenExtent(atElevation(elevation, azimuth, 800.0f), 95.0f);
            CHECK(extent.y == Catch::Approx(extent.x).epsilon(0.01));
            CHECK(extent.x > 0.0f);
        }
    }
}

TEST_CASE("A sky body's disc keeps its size wherever it stands", "[sky]") {
    // Seen straight on, the same size at the horizon as overhead.
    const float low = screenExtent(atElevation(5.0f, 45.0f, 800.0f), 95.0f).x;
    const float high = screenExtent(atElevation(80.0f, 45.0f, 800.0f), 95.0f).x;
    CHECK(high == Catch::Approx(low).epsilon(0.01));
}
