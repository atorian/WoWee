// What is and is not a gather.
//
// The rank tables themselves are data, and copying them into a test proves
// only that the copy matches. What is tested here is the rule around them: a
// spell that is not a gather is never taken for one, and only a profession's
// base id names its ranks. Which rank a node is gathered with is behaviour of
// the game, and belongs in a GameHarness test once the harness can place game
// objects.
#include <catch_amalgamated.hpp>

#include "game/gather_spells.hpp"

using namespace wowee::game;

TEST_CASE("a spell that is not a gather is not one", "[gather]") {
    // 133 is Fireball, 2018 is Blacksmithing - a profession, but not a gather.
    for (uint32_t id : {uint32_t{0}, uint32_t{133}, uint32_t{2018}, uint32_t{29355}}) {
        INFO("spell " << id);
        CHECK_FALSE(isGatherRank(id));
        CHECK_FALSE(isMiningRank(id));
        CHECK_FALSE(isHerbRank(id));
    }
}

TEST_CASE("anything but a base id selects nothing", "[gather]") {
    // Including a higher rank: only the base names a profession, which is the
    // convention the callers already use.
    size_t count = 99;
    CHECK(gatherRanksForBase(0, count) == nullptr);
    CHECK(count == 0);
    count = 99;
    CHECK(gatherRanksForBase(2576, count) == nullptr);
    CHECK(count == 0);
}
