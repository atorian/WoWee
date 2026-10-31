// Where the sun is over the day, and where it lands on the screen.
#include <catch_amalgamated.hpp>

#include "rendering/sun_direction.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::sunLightDirection;
using wowee::rendering::sunScreenPosition;

// The sun shafts stream from where this puts the sun, and every full-screen pass
// samples with (0,0) at the top left. A sun that came out mirrored top to bottom
// would pour its rays up out of the ground.
TEST_CASE("the sun lands on screen where it is in the sky", "[sky][sun]") {
    const glm::vec3 eye(-8913.0f, 554.0f, 94.0f);
    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f));
    // As the camera builds it: Y flipped for Vulkan.
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;

    // Dead ahead is the middle of the screen, wherever the camera stands.
    const auto ahead = sunScreenPosition(view, proj, forward);
    REQUIRE(ahead.inFront);
    CHECK(ahead.uv.x == Catch::Approx(0.5f).margin(1e-5f));
    CHECK(ahead.uv.y == Catch::Approx(0.5f).margin(1e-5f));

    // Ahead and a little up is above the middle - smaller v, toward the top.
    const auto raised = sunScreenPosition(view, proj, glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f)));
    REQUIRE(raised.inFront);
    CHECK(raised.uv.y < 0.5f);
    CHECK(raised.uv.x == Catch::Approx(0.5f).margin(1e-5f));

    // Behind the camera it has no place on screen at all.
    CHECK_FALSE(sunScreenPosition(view, proj, -forward).inFront);
}

// The client's sun never sets: its light comes down at every hour, so flat
// ground is always directly lit and the evening is made by the light bands'
// colour. The sun was being walked under the ground at six in the evening
// instead, and from then on the ground took the ambient band alone - a dusk
// sky over streets as dark as midnight while the client showed them lit.
TEST_CASE("the sun's light comes down on flat ground at every hour", "[sky][sun]") {
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    for (int halfHour = 0; halfHour < 48; ++halfHour) {
        const float hour = halfHour * 0.5f;
        const glm::vec3 dir = sunLightDirection(hour / 24.0f);
        INFO("hour " << hour);
        CHECK(glm::length(dir) == Catch::Approx(1.0f));
        // Lambert on flat ground: a third of the direct light at the least.
        CHECK(glm::dot(-dir, up) > 0.3f);
    }
}

TEST_CASE("half past eight in the evening lights the ground as the client does", "[sky][sun]") {
    // The hour of the report: a ground that had gone dark under a sunset sky.
    const glm::vec3 dir = sunLightDirection(20.5f / 24.0f);
    // Light coming down: the sun is up, as everything that gates on its
    // height expects.
    CHECK(dir.z < -0.3f);
}

TEST_CASE("the sun stands lower at dawn and dusk than at noon", "[sky][sun]") {
    // Hills keep their shape through the day: the light rakes across them in
    // the morning and the evening and comes down more steeply at noon.
    const float dawn = -sunLightDirection(6.0f / 24.0f).z;
    const float noon = -sunLightDirection(12.0f / 24.0f).z;
    const float dusk = -sunLightDirection(18.0f / 24.0f).z;
    CHECK(dawn < noon);
    CHECK(dusk < noon);
    // And always from the same quarter of the sky, so the shadows of a
    // building fall the same way all day, as the client's do.
    const glm::vec2 dawnHeading = glm::normalize(glm::vec2(sunLightDirection(6.0f / 24.0f)));
    const glm::vec2 noonHeading = glm::normalize(glm::vec2(sunLightDirection(12.0f / 24.0f)));
    CHECK(glm::dot(dawnHeading, noonHeading) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("the sun's path is continuous across midnight", "[sky][sun]") {
    // The tables wrap: the moment before midnight and the moment after are
    // the same sky, not a jump from the last key to the first.
    const glm::vec3 before = sunLightDirection(23.99f / 24.0f);
    const glm::vec3 after = sunLightDirection(0.01f / 24.0f);
    CHECK(glm::distance(before, after) < 0.01f);
    // And a day on from any hour is that hour again.
    CHECK(glm::distance(sunLightDirection(0.3f), sunLightDirection(1.3f)) < 1e-5f);
}

TEST_CASE("a day table reads between its keys and around the day's end", "[sky][sun]") {
    using wowee::rendering::DayKey;
    using wowee::rendering::interpolateDayTable;
    const DayKey table[] = {{0.0f, 1.0f}, {0.25f, 0.0f}, {0.5f, 0.0f}, {0.75f, 1.0f}};
    // On a key, that key's value.
    CHECK(interpolateDayTable(table, 0.25f) == Catch::Approx(0.0f));
    // Halfway between two keys, halfway between their values.
    CHECK(interpolateDayTable(table, 0.125f) == Catch::Approx(0.5f));
    // Past the last key the table runs on to the first key of the next day:
    // 0.875 is halfway from the 1.0 at 0.75 to the 1.0 at the next 0.0.
    CHECK(interpolateDayTable(table, 0.875f) == Catch::Approx(1.0f));
    const DayKey ramp[] = {{0.0f, 0.0f}, {0.75f, 1.0f}};
    CHECK(interpolateDayTable(ramp, 0.875f) == Catch::Approx(0.5f));
}
