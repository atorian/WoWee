#include "catch_amalgamated.hpp"
#include "rendering/lightning_storm.hpp"

using namespace wowee::rendering;
namespace {
const glm::vec3 eye(0, 0, 2);
auto flatGround = [](const glm::vec3&) { return std::optional<float>(0.0f); };
}
TEST_CASE("A heavy storm leaves long quiet intervals between world strikes", "[lightning]") {
    LightningStorm storm(19);
    storm.setWeather(true, 1.0f);
    int strikes = 0;
    bool wasVisible = false;
    float quiet = 0;
    for (int i = 0; i < 6000; ++i) {
        storm.update(0.05f, eye, flatGround);
        if (storm.visible() && !wasVisible) {
            CHECK(quiet > 12.0f);
            ++strikes;
            quiet = 0;
        }
        if (!storm.visible()) quiet += 0.05f;
        wasVisible = storm.visible();
        (void)storm.takeThunder();
    }
    CHECK(strikes >= 8);
    CHECK(strikes <= 18);
}
TEST_CASE("The leader reaches the ground before the light pulse and leaves a fading trail", "[lightning]") {
    LightningStorm storm(1);
    storm.strike({0, 0, 150}, {0, 0, 0});
    storm.update(0.04f, eye, flatGround);
    CHECK(storm.progress() > 0);
    CHECK(storm.progress() < 1);
    CHECK(storm.illumination() == 0);
    storm.update(0.06f, eye, flatGround);
    CHECK(storm.progress() == 1);
    const float peak = storm.brightness();
    CHECK(storm.illumination() > 0.8f);
    storm.update(0.4f, eye, flatGround);
    REQUIRE(storm.visible());
    CHECK(storm.brightness() < peak);
    CHECK(storm.brightness() > 0.05f);
    const float trail = storm.brightness();
    storm.update(0.4f, eye, flatGround);
    CHECK(storm.brightness() < trail);
    storm.update(1.0f, eye, flatGround);
    CHECK_FALSE(storm.visible());
    CHECK(storm.illumination() == 0);
}
TEST_CASE("Thunder travels from the strike and is delivered only once", "[lightning]") {
    LightningStorm storm(3);
    storm.strike({375, 0, 150}, {375, 0, 0});
    storm.update(0.5f, eye, flatGround);
    CHECK(storm.takeThunder().empty());
    storm.update(0.7f, eye, flatGround);
    const auto arrivals = storm.takeThunder();
    REQUIRE(arrivals.size() == 1);
    CHECK(arrivals[0].position == glm::vec3(375, 0, 0));
    CHECK(storm.takeThunder().empty());
    storm.update(5.0f, eye, flatGround);
    CHECK(storm.takeThunder().empty());
}
TEST_CASE("Strikes terminate at supplied world ground with independently connected branches", "[lightning]") {
    LightningStorm storm(7);
    storm.setWeather(true, 1);
    for (int i = 0; i < 800 && !storm.visible(); ++i)
        storm.update(0.05f, eye, [](const glm::vec3&) { return std::optional<float>(37.0f); });
    REQUIRE(storm.visible());
    CHECK(storm.groundPosition().z == 37.0f);
    REQUIRE_FALSE(storm.segments().empty());
    CHECK(storm.segments().back().end == storm.groundPosition());
    for (const auto& segment : storm.segments()) {
        CHECK(glm::length(segment.end - segment.start) < 45.0f);
        CHECK(std::isfinite(segment.start.x));
    }
}
TEST_CASE("Clear weather finishes an existing strike but never schedules another", "[lightning]") {
    LightningStorm storm(2);
    storm.setWeather(true, 1);
    storm.strike({100, 0, 150}, {100, 0, 0});
    storm.setWeather(false, 0);
    storm.update(0.5f, eye, flatGround);
    CHECK(storm.visible());
    CHECK(storm.takeThunder().size() == 1);
    storm.update(90, eye, flatGround);
    CHECK_FALSE(storm.visible());
    CHECK(storm.takeThunder().empty());
}
TEST_CASE("Unloaded ground and teleports do not leave invented strikes or stale thunder", "[lightning]") {
    LightningStorm storm(4);
    storm.setWeather(true, 1);
    storm.update(50, eye, [](const glm::vec3&) { return std::optional<float>(); });
    CHECK_FALSE(storm.visible());
    storm.strike({100, 0, 150}, {100, 0, 0});
    storm.update(0.01f, {5000, 0, 2}, flatGround);
    CHECK_FALSE(storm.visible());
    storm.update(2, {5000, 0, 2}, flatGround);
    CHECK(storm.takeThunder().empty());
}
