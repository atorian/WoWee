// Combat, end to end: what the server says of a fight and what the rest of
// the client is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/game_events.hpp"

#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

TEST_CASE("honor credited for a kill is announced with the victim and their rank", "[combat][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::HonorGained> gained;
    auto sub = h.events().subscribe<ev::HonorGained>([&](const ev::HonorGained& e) { gained.push_back(e); });

    // SMSG_PVP_CREDIT: the honor, the victim, the victim's PvP rank.
    h.receive(Opcode::SMSG_PVP_CREDIT, Body().u32(21).u64(0x0000000000004242ull).u32(7).bytes());
    h.pump();

    REQUIRE(gained.size() == 1);
    CHECK(gained[0].honor == 21);
    CHECK(gained[0].victimGuid == 0x4242ull);
    CHECK(gained[0].victimRank == 7);
}
