// Which water reached the screen, as the GPU's occlusion answers say: what the
// reflection is chosen from.
#include <catch_amalgamated.hpp>

#include "rendering/water_visibility.hpp"

using wowee::rendering::WaterVisibility;

TEST_CASE("water nobody has asked about yet counts as seen", "[water][visibility]") {
    // Water that has just come into view reflects from its first frame.
    WaterVisibility visibility;
    CHECK(visibility.seen(7));
}

TEST_CASE("water none of whose samples reached the screen is hidden", "[water][visibility]") {
    WaterVisibility visibility;
    visibility.answer(7, 0);
    CHECK_FALSE(visibility.seen(7));
    CHECK(visibility.seen(8));
}

TEST_CASE("water that shows again is seen again", "[water][visibility]") {
    WaterVisibility visibility;
    visibility.answer(7, 0);
    visibility.answer(7, 12);
    CHECK(visibility.seen(7));
}

TEST_CASE("water removed and its id given out again is new water", "[water][visibility]") {
    WaterVisibility visibility;
    visibility.answer(7, 0);
    visibility.forget(7);
    CHECK(visibility.seen(7));
}
