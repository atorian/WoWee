#include <catch_amalgamated.hpp>

#include <string>

#include "game/reputation_standing.hpp"

using namespace wowee::game;

// These thresholds used to be written out twice - once for the panel this
// client draws and once for the original interface's GetFactionInfo. They
// agreed only because someone checked, and the top of the last band did not:
// exalted was given the width of revered, so a faction at exalted drew an
// almost empty bar.
//
// The expected values are WoW's own standings, numbered 1..8 as
// GetFactionInfo hands them to the interface, which indexes its colours and
// names by that number.

TEST_CASE("each value falls in WoW's standing for it, at both edges", "[reputation]") {
    struct Edge { int value; int id; const char* name; };
    const Edge edges[] = {
        {-42000, 1, "Hated"},    {-6001, 1, "Hated"},
        {-6000, 2, "Hostile"},   {-3001, 2, "Hostile"},
        {-3000, 3, "Unfriendly"}, {-1, 3, "Unfriendly"},
        {0, 4, "Neutral"},       {2999, 4, "Neutral"},
        {3000, 5, "Friendly"},   {8999, 5, "Friendly"},
        {9000, 6, "Honored"},    {20999, 6, "Honored"},
        {21000, 7, "Revered"},   {41999, 7, "Revered"},
        {42000, 8, "Exalted"},   {42999, 8, "Exalted"},
    };
    for (const Edge& e : edges) {
        INFO("value " << e.value);
        const auto& standing = reputationStandingFor(e.value);
        CHECK(standing.id == e.id);
        CHECK(std::string(standing.name) == e.name);
    }
}

TEST_CASE("Exalted is a thousand wide, not open-ended", "[reputation]") {
    // The bar reads out of a thousand. Giving exalted the width of revered
    // drew a faction that had earned it as though it had barely started.
    const auto& exalted = reputationStandingFor(42000);
    CHECK(exalted.floor == 42000);
    CHECK(exalted.ceiling == 42999);
}

TEST_CASE("Below hated is still hated", "[reputation]") {
    // The server does not send lower, but a value that fell through every band
    // would otherwise be answered with whatever the loop ended on.
    REQUIRE(reputationStandingFor(-99999).id == 1);
    REQUIRE(reputationStandingFor(kReputationStandings[0].floor - 1).id == 1);
}

TEST_CASE("Neutral is where a faction with no history sits", "[reputation]") {
    REQUIRE(reputationStandingFor(0).id == 4);
    REQUIRE(std::string(reputationStandingFor(0).name) == "Neutral");
}

TEST_CASE("a standing's name by an item's rank index", "[reputation]") {
    // Two id spaces, one apart: the interface numbers standings 1..8 and an
    // item's requiredReputationRank is 0..7. Indexing with the wrong one gives
    // an item that says Revered where it means Honored - as plausible a
    // sentence as the right one, which is why this is pinned.
    CHECK(std::string(reputationRankName(0)) == "Hated");
    CHECK(std::string(reputationRankName(3)) == "Neutral");
    CHECK(std::string(reputationRankName(7)) == "Exalted");

    SECTION("every rank, in order") {
        const char* names[] = {"Hated", "Hostile", "Unfriendly", "Neutral",
                               "Friendly", "Honored", "Revered", "Exalted"};
        for (uint32_t i = 0; i < 8; ++i) {
            INFO("index " << i);
            CHECK(std::string(reputationRankName(i)) == names[i]);
        }
    }

    SECTION("a rank past the table says so rather than reading past it") {
        CHECK(std::string(reputationRankName(8)) == "Unknown");
        CHECK(std::string(reputationRankName(4000)) == "Unknown");
    }
}
