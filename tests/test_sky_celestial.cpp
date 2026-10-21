// The sun as WoW.exe 3.3.5a shows it, held to the client's hours.
//
// Where the disc stands, when it is above the horizon, when its glare shows
// and how strongly, as read from the client (sky_celestial.hpp). The renderer
// had a fixed 05:00-19:00 window for the disc under a light that never sets,
// so an evening sky glowed round a sun that was not drawn.
#include <catch_amalgamated.hpp>

#include "rendering/sky_celestial.hpp"
#include "rendering/cloud_coverage.hpp"

#include <cmath>

namespace sc = wowee::rendering::sky_celestial;

namespace {

float dayAt(float hours) { return hours / 24.0f; }

float elevationDegrees(const glm::vec3& dir) {
    return glm::degrees(std::asin(std::clamp(dir.z, -1.0f, 1.0f)));
}

}  // namespace

TEST_CASE("The client's sky cosine is a cosine", "[sky_celestial]") {
    for (float x = -2.0f; x <= 2.0f; x += 0.0625f) {
        CAPTURE(x);
        CHECK(sc::cosPi(x) == Catch::Approx(std::cos(3.14159265f * x)).margin(0.03));
    }
}

TEST_CASE("The sun is above the horizon by day and under it by night", "[sky_celestial]") {
    // Up between about 06:10 and 20:30; ten degrees under from 21:30 to 05:30.
    for (const float hour : {0.0f, 3.0f, 5.5f, 21.5f, 22.0f, 23.5f}) {
        CAPTURE(hour);
        CHECK_FALSE(sc::discAboveHorizon(sc::sunDisc(dayAt(hour))));
    }
    // At 21:00 the centre has set and only the top of the large evening disc
    // is left above the horizon.
    const sc::SkyDisc nine = sc::sunDisc(dayAt(21.0f));
    CHECK(elevationDegrees(nine.direction) < 0.0f);
    CHECK(sc::discAboveHorizon(nine));
    for (const float hour : {7.0f, 9.0f, 12.0f, 16.5f, 19.5f}) {
        CAPTURE(hour);
        CHECK(sc::discAboveHorizon(sc::sunDisc(dayAt(hour))));
        CHECK(elevationDegrees(sc::sunDisc(dayAt(hour)).direction) > 0.0f);
    }
}

TEST_CASE("The sun stands nearly overhead at midday and is larger at its ends",
          "[sky_celestial]") {
    const sc::SkyDisc noon = sc::sunDisc(dayAt(12.0f));
    CHECK(elevationDegrees(noon.direction) > 80.0f);
    // Twice the size at sunrise and sunset as through the day.
    CHECK(sc::sunDisc(dayAt(6.0f)).scale == Catch::Approx(2.0f * noon.scale));
    CHECK(sc::sunDisc(dayAt(21.0f)).scale == Catch::Approx(2.0f * noon.scale));
}

TEST_CASE("The sun rises and sets on the same side of the sky", "[sky_celestial]") {
    const glm::vec3 morning = sc::sunDisc(dayAt(7.0f)).direction;
    const glm::vec3 evening = sc::sunDisc(dayAt(19.5f)).direction;
    const glm::vec2 a = glm::normalize(glm::vec2(morning));
    const glm::vec2 b = glm::normalize(glm::vec2(evening));
    CHECK(glm::dot(a, b) == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("The disc fades in over the last stretch above the horizon", "[sky_celestial]") {
    // A disc whose centre sits on the horizon: its bottom half is hidden, its
    // top edge has 0.5 units of clearance and shows in full only above 0.4.
    sc::SkyDisc onHorizon;
    onHorizon.horizonHeight = 0.0f;
    onHorizon.scale = 1.0f;
    CHECK(sc::discHorizonAlpha(onHorizon, -0.25f) == 0.0f);
    CHECK(sc::discHorizonAlpha(onHorizon, 0.2f) == Catch::Approx(0.5f));
    CHECK(sc::discHorizonAlpha(onHorizon, 0.5f) == 1.0f);
    // High in the sky none of it is cut.
    CHECK(sc::discHorizonAlpha(sc::sunDisc(dayAt(12.0f)), -0.5f) == 1.0f);
}

TEST_CASE("The glare shows from mid-morning to the evening", "[sky_celestial]") {
    CHECK(sc::lookup(sc::kSunGlareHours, dayAt(5.0f)) == 0.0f);
    CHECK(sc::lookup(sc::kSunGlareHours, dayAt(7.0f)) == Catch::Approx(0.5f).margin(0.01));
    CHECK(sc::lookup(sc::kSunGlareHours, dayAt(12.0f)) == 1.0f);
    CHECK(sc::lookup(sc::kSunGlareHours, dayAt(20.25f)) == Catch::Approx(0.5f).margin(0.01));
    CHECK(sc::lookup(sc::kSunGlareHours, dayAt(21.5f)) == 0.0f);
}

TEST_CASE("Weather puts out the sun once it is a quarter of the way in", "[sky_celestial]") {
    CHECK(sc::weatherFade(0.0f) == 1.0f);
    CHECK(sc::weatherFade(0.125f) == Catch::Approx(0.5f));
    CHECK(sc::weatherFade(0.25f) == 0.0f);
    CHECK(sc::weatherFade(0.8f) == 0.0f);
}

TEST_CASE("The glare grows and brightens as the view turns onto the sun", "[sky_celestial]") {
    const sc::GlareShape aside = sc::sunGlareShape(0.2f);
    const sc::GlareShape onto = sc::sunGlareShape(1.0f);
    CHECK(onto.size > 6.0f * aside.size);
    CHECK(onto.alpha == Catch::Approx(2.0f * aside.alpha));
    // Nothing changes until the sun is within about 45 degrees of the view.
    CHECK(sc::sunGlareShape(0.5f).size == aside.size);
}

TEST_CASE("The glare comes up in a quarter second and goes in two thirds", "[sky_celestial]") {
    float v = 0.0f;
    for (int i = 0; i < 25; ++i) v = sc::easeGlare(sc::kSunGlare, v, 1.0f, 0.01f);
    CHECK(v == Catch::Approx(1.0f));
    for (int i = 0; i < 33; ++i) v = sc::easeGlare(sc::kSunGlare, v, 0.0f, 0.01f);
    CHECK(v > 0.0f);
    for (int i = 0; i < 34; ++i) v = sc::easeGlare(sc::kSunGlare, v, 0.0f, 0.01f);
    CHECK(v == 0.0f);
}

TEST_CASE("The moon is up around midnight on the sun's side of the sky", "[sky_celestial]") {
    // Up from about 22:18 to 03:24, under the horizon all day.
    for (const float hour : {23.0f, 0.0f, 1.0f, 2.5f}) {
        CAPTURE(hour);
        CHECK(sc::discAboveHorizon(sc::moonDisc(dayAt(hour))));
    }
    for (const float hour : {5.0f, 12.0f, 18.0f, 21.5f}) {
        CAPTURE(hour);
        CHECK_FALSE(sc::discAboveHorizon(sc::moonDisc(dayAt(hour))));
    }
    // About 55 degrees up at midnight (56 through the client's cubic cosine),
    // on the azimuth the sun rises and sets on.
    const glm::vec3 midnight = sc::moonDisc(dayAt(0.0f)).direction;
    CHECK(elevationDegrees(midnight) == Catch::Approx(55.0f).margin(1.5f));
    const glm::vec3 sunrise = sc::sunDisc(dayAt(7.0f)).direction;
    CHECK(glm::dot(glm::normalize(glm::vec2(midnight)), glm::normalize(glm::vec2(sunrise))) ==
          Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("The moon's glare shows round midnight and is twice the moon", "[sky_celestial]") {
    CHECK(sc::lookup(sc::kMoonGlareHours, dayAt(0.0f)) == 1.0f);
    CHECK(sc::lookup(sc::kMoonGlareHours, dayAt(12.0f)) == 0.0f);
    CHECK(sc::lookup(sc::kMoonGlareHours, dayAt(4.0f)) == 0.0f);
    CHECK(sc::lookup(sc::kMoonGlareHours, dayAt(22.0f)) == 0.0f);
    const float scale = sc::moonDisc(dayAt(0.0f)).scale;
    // The same size at any angle, unlike the sun's.
    CHECK(sc::moonGlareShape(0.2f, scale).size == Catch::Approx(2.0f * scale));
    CHECK(sc::moonGlareShape(1.0f, scale).size == Catch::Approx(2.0f * scale));
    CHECK(sc::moonGlareShape(1.0f, scale).alpha == Catch::Approx(1.0f));
    CHECK(sc::moonGlareShape(0.2f, scale).alpha == Catch::Approx(0.1f));
    // Brightest through half cover; none through clear sky or thick cloud.
    CHECK(sc::moonGlareThroughCloud(0.5f) == 1.0f);
    CHECK(sc::moonGlareThroughCloud(0.0f) == 0.0f);
    CHECK(sc::moonGlareThroughCloud(1.0f) == 0.0f);
    CHECK(sc::sunGlareThroughCloud(0.0f) == 1.0f);
}

TEST_CASE("Looking straight at the sun dims the scene's light by a third", "[sky_celestial]") {
    CHECK(sc::lookingAtSunDimming(1.0f, 1.0f) == Catch::Approx(0.35f));
    CHECK(sc::lookingAtSunDimming(1.0f, 0.0f) == 0.0f);
    // Looking away it is all but nothing.
    CHECK(sc::lookingAtSunDimming(0.0f, 1.0f) < 0.011f);
}

TEST_CASE("Cloud cover leaves clear gaps and becomes overcast with the weather", "[clouds]") {
    namespace cc = wowee::rendering::cloud_coverage;
    int gaps = 0, patches = 0;
    float clearSum = 0.0f, stormSum = 0.0f;
    for (int y = -16; y <= 16; ++y) {
        for (int x = -16; x <= 16; ++x) {
            const glm::vec3 ray = glm::normalize(glm::vec3(x / 8.0f, y / 8.0f, 1.0f));
            const float clear = cc::coverageToward(ray, 0.5f, 0.0f);
            const float storm = cc::coverageToward(ray, 0.95f, 0.0f);
            REQUIRE(cc::coverageToward(ray, 0.0f, 0.0f) == 0.0f);
            REQUIRE(storm >= clear);
            REQUIRE(storm <= 1.0f);
            clearSum += clear;
            stormSum += storm;
            gaps += clear < 0.01f;
            patches += clear > 0.25f;
        }
    }
    // These are visible behaviours, not a particular noise seed or pixel image.
    CHECK(gaps > 100);
    CHECK(patches > 100);
    CHECK(stormSum / 1089.0f > 0.85f);
    CHECK(stormSum > clearSum * 2.0f);
}

TEST_CASE("Cloud animation is continuous and disappears at the horizon", "[clouds]") {
    namespace cc = wowee::rendering::cloud_coverage;
    float motion = 0.0f;
    for (int x = -15; x <= 15; ++x) {
        const glm::vec3 ray = glm::normalize(glm::vec3(x * 0.17f, 0.4f, 1.0f));
        const float a = cc::coverageToward(ray, 0.5f, 0.0f);
        CHECK(std::abs(cc::coverageToward(ray, 0.5f, 0.0001f) - a) < 0.01f);
        motion += std::abs(cc::coverageToward(ray, 0.5f, 1.0f) - a);
    }
    CHECK(motion > 0.1f);
    CHECK(cc::coverageToward(glm::vec3(1, 0, 0), 1.0f, 0.0f) == 0.0f);
    CHECK(cc::coverageToward(glm::normalize(glm::vec3(1, 0, -0.1f)), 1.0f, 0.0f) == 0.0f);
}

TEST_CASE("Clear daylight cloud edges retain their illumination", "[clouds]") {
    namespace cc = wowee::rendering::cloud_coverage;
    int edges = 0;
    float darkest = 1.0f, sum = 0.0f;
    for (int y = -12; y <= 12; ++y) {
        for (int x = -12; x <= 12; ++x) {
            const glm::vec3 ray = glm::normalize(glm::vec3(x / 16.0f, y / 16.0f, 1.0f));
            const auto shape = cc::cloudShapeAt(ray, 0.5f, 0.0f);
            if (shape.cloud < 0.05f || shape.cloud > 0.5f) continue;
            // Isolate sunlight: no ambient fill can hide an erroneously dark rim.
            const auto lit = cc::cloudLighting(shape, ray, glm::vec3(0, 0, 1), 0.0f,
                                               glm::vec3(1), glm::vec3(0), glm::vec3(0));
            darkest = std::min(darkest, lit.r);
            sum += lit.r;
            ++edges;
        }
    }
    REQUIRE(edges > 20);
    CHECK(darkest > 0.75f);
    CHECK(sum / edges > 0.9f);
}
