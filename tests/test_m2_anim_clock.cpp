#include <catch_amalgamated.hpp>

#include "rendering/m2_anim_clock.hpp"

using wowee::rendering::m2AnimTimeAt;

TEST_CASE("an unstepped instance loops at its sequence length", "[m2][animclock]") {
    // A waterfall with a 1 s scroll, 2.5 s after it started, is half way
    // through its third lap.
    REQUIRE(m2AnimTimeAt(2500.0, 0.0, 1000.0f, false, false) == Catch::Approx(500.0f));
}

TEST_CASE("an unstepped emitter with no sequence loops at 3333 ms", "[m2][animclock]") {
    REQUIRE(m2AnimTimeAt(7000.0, 0.0, 0.0f, false, true) == Catch::Approx(334.0f));
}

TEST_CASE("a stepped instance is not wrapped here", "[m2][animclock]") {
    // A door held at the end of its 1 s open sequence shows exactly the end.
    REQUIRE(m2AnimTimeAt(5000.0, 4000.0, 1000.0f, true, false) == Catch::Approx(1000.0f));
}

TEST_CASE("the time is measured from the instance's own start", "[m2][animclock]") {
    REQUIRE(m2AnimTimeAt(10000.0, 9750.0, 1000.0f, false, false) == Catch::Approx(250.0f));
}

TEST_CASE("half a millisecond survives ten hours of clock", "[m2][animclock]") {
    // 36,000,000 ms in: a float clock has a spacing of 4 ms there.
    REQUIRE(m2AnimTimeAt(36000000.5, 36000000.0, 0.0f, false, false) == 0.5f);
}
