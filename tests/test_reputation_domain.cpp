// Reputation, end to end: the standings the server sends at login and as they
// move, and what the reputation panel and the at-war checks read.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/reputation_handler.hpp"

#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

/// SMSG_INITIALIZE_FACTIONS: how many, then a flags byte and the earned
/// standing for each, by ReputationListID.
std::vector<uint8_t> initializeFactions(const std::vector<std::pair<uint8_t, int32_t>>& factions) {
    Body b;
    b.u32(static_cast<uint32_t>(factions.size()));
    for (const auto& [flags, standing] : factions) b.u8(flags).u32(static_cast<uint32_t>(standing));
    return b.bytes();
}

constexpr uint8_t kVisible = 0x01;
constexpr uint8_t kAtWar = 0x02;

}  // namespace

TEST_CASE("the standings at login are what the reputation reads", "[reputation][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS,
              initializeFactions({{kVisible, 100}, {kVisible | kAtWar, -3000}}));
    h.pump();

    const auto& factions = h.game().reputation().factions();
    REQUIRE(factions.size() == 2);
    CHECK(factions[1].standing == -3000);
    CHECK_FALSE(h.game().isFactionAtWar(0));
    CHECK(h.game().isFactionAtWar(1));

    // SMSG_SET_FACTION_ATWAR: the faction, then war or peace.
    h.receive(Opcode::SMSG_SET_FACTION_ATWAR, Body().u32(1).u8(0));
    h.pump();
    CHECK_FALSE(h.game().isFactionAtWar(1));
}

TEST_CASE("the next character has none of the last one's reputation", "[reputation][harness]") {
    PlayerSpec alice;
    alice.guid = 0x81;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x82;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible | kAtWar, -3000}}));
    h.pump();
    h.game().setWatchedFactionId(72);
    REQUIRE(h.game().isFactionAtWar(0));

    h.relogAs(bob);

    // Until Bob's own standings arrive, Alice's at-war flags judged who was
    // hostile to him, and her watched faction was on his reputation bar.
    CHECK_FALSE(h.game().isFactionAtWar(0));
    CHECK(h.game().reputation().factions().empty());
    CHECK(h.game().getWatchedFactionId() == 0);
}
