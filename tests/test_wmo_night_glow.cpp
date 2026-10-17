// When a building's night-lit materials glow.
//
// Windows were drawn as glass of our own making - darkened to a third, mixed
// with a reflection of the sky - and the stained glass of Dalaran came out as
// dark panes. WoW.exe draws them as the textured surface they are, and after
// dark adds the material's F_SIDN colour to its light. These are the hours of
// that glow as the client keeps them.
#include <catch_amalgamated.hpp>

#include "rendering/wmo_night_glow.hpp"

using wowee::rendering::wmoNightGlow;

namespace {
float atHour(float hours) { return wmoNightGlow(hours / 24.0f); }
}  // namespace

TEST_CASE("windows do not glow by day", "[wmo-night-glow]") {
    for (float h : {7.0f, 9.0f, 12.0f, 15.0f, 18.0f, 20.5f}) {
        CHECK(atHour(h) == 0.0f);
    }
}

TEST_CASE("windows glow through the night, midnight included",
          "[wmo-night-glow]") {
    for (float h : {21.5f, 23.0f, 0.0f, 2.0f, 5.9f}) {
        CHECK(atHour(h) == Catch::Approx(254.0f / 256.0f).margin(1.0 / 256.0));
    }
}

TEST_CASE("the glow fades out after dawn and comes up after dusk",
          "[wmo-night-glow]") {
    // Out over 06:00 to 07:00, falling all the way.
    CHECK(atHour(6.25f) > atHour(6.5f));
    CHECK(atHour(6.5f) > atHour(6.75f));
    CHECK(atHour(6.5f) == Catch::Approx(0.5f).margin(0.01f));
    // In over 20:30 to 21:30, rising all the way.
    CHECK(atHour(20.75f) < atHour(21.0f));
    CHECK(atHour(21.0f) < atHour(21.25f));
    CHECK(atHour(21.0f) == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("a day fraction past the end of the day wraps", "[wmo-night-glow]") {
    CHECK(wmoNightGlow(1.5f) == wmoNightGlow(0.5f));
    CHECK(wmoNightGlow(-0.5f) == wmoNightGlow(0.5f));
}
