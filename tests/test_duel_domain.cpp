// Duels, end to end: the server's duel packets, what the interface is told
// and shown, and the answers that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "duel/services/duels.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kThrall = 0x20;
constexpr uint64_t kDuelFlag = 0xF110000000000099ull;

/// SMSG_NAME_QUERY_RESPONSE for a player who exists.
std::vector<uint8_t> nameResponse(uint64_t guid, const std::string& name) {
    return Body().packedGuid(guid).u8(0 /* found */).cstr(name).cstr("").u8(2).u8(0).u8(7);
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

}  // namespace

TEST_CASE("a duel challenge names the challenger, and accepting names the flag", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kThrall, "Thrall"));
    h.pump();

    h.receive(Opcode::SMSG_DUEL_REQUESTED, Body().u64(kDuelFlag).u64(kThrall));
    h.pump();

    const auto requested = h.addonEvents("DUEL_REQUESTED");
    REQUIRE(requested.size() == 1);
    CHECK(requested[0].args.at(0) == "Thrall");
    CHECK(chatSaid(h, "Thrall challenges you to a duel!"));

    h.duels().acceptDuel();
    h.duels().acceptDuel();
    auto* accepted = h.lastSent(Opcode::CMSG_DUEL_ACCEPTED);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->readUInt64() == kDuelFlag);
    CHECK(h.countSent(Opcode::CMSG_DUEL_ACCEPTED) == 1);
    CHECK(chatSaid(h, "You accept the duel."));
}

TEST_CASE("the player's own challenge asks nothing of them, and can be called off", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();

    // The server sends the challenge to both duellists; this one issued it.
    h.receive(Opcode::SMSG_DUEL_REQUESTED, Body().u64(kDuelFlag).u64(PlayerSpec{}.guid));
    h.pump();

    CHECK(h.addonEvents("DUEL_REQUESTED").empty());
    h.duels().acceptDuel();
    CHECK(h.countSent(Opcode::CMSG_DUEL_ACCEPTED) == 0);
    h.duels().forfeitDuel();
    auto* cancelled = h.lastSent(Opcode::CMSG_DUEL_CANCELLED);
    REQUIRE(cancelled != nullptr);
    CHECK(cancelled->readUInt64() == kDuelFlag);
    CHECK(chatSaid(h, "You have forfeited the duel."));
}

TEST_CASE("the countdown runs until the duel ends", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();
    CHECK(h.duels().getDuelCountdownRemaining() == 0.0f);

    h.receive(Opcode::SMSG_DUEL_COUNTDOWN, Body().u32(3000));
    h.pump();
    CHECK(h.duels().getDuelCountdownRemaining() > 2.0f);

    h.receive(Opcode::SMSG_DUEL_COMPLETE, Body().u8(1 /* it had started */));
    h.pump();
    CHECK(h.duels().getDuelCountdownRemaining() == 0.0f);
    CHECK(h.addonEvents("DUEL_FINISHED").size() == 1);
    CHECK_FALSE(chatSaid(h, "The duel was cancelled."));
}

TEST_CASE("a duel called off before it starts says so", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_DUEL_COMPLETE, Body().u8(0));
    h.pump();

    CHECK(chatSaid(h, "The duel was cancelled."));
    CHECK(h.addonEvents("DUEL_FINISHED").size() == 1);
}

TEST_CASE("the winner is announced, and a duellist who ran is said to have fled", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_DUEL_WINNER, Body().u8(0).cstr("Thrall").cstr("Jaina"));
    h.receive(Opcode::SMSG_DUEL_WINNER, Body().u8(1).cstr("Jaina").cstr("Thrall"));
    h.pump();

    CHECK(chatSaid(h, "Thrall has defeated Jaina in a duel!"));
    CHECK(chatSaid(h, "Thrall has fled from the duel. Jaina wins!"));
}

TEST_CASE("leaving the duel area warns once and starts the interface's countdown", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_DUEL_OUTOFBOUNDS, Body());
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args == std::vector<std::string>{"You are out of the duel area!"});
    CHECK(h.addonEvents("DUEL_OUTOFBOUNDS").size() == 1);

    h.receive(Opcode::SMSG_DUEL_INBOUNDS, Body());
    h.pump();
    CHECK(h.addonEvents("DUEL_INBOUNDS").size() == 1);
}

TEST_CASE("a challenge needs someone to challenge", "[duel][harness]") {
    GameHarness h;
    h.enterWorld();

    h.duels().proposeDuel(0);
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_CAST_SPELL));
    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args == std::vector<std::string>{"You must target a player to challenge to a duel."});

    // A duel is the Duel spell cast at the one challenged.
    h.duels().proposeDuel(kThrall);
    auto* cast = h.lastSent(Opcode::CMSG_CAST_SPELL);
    REQUIRE(cast != nullptr);
    cast->readUInt8();  // cast count
    CHECK(cast->readUInt32() == 7266u);
    CHECK(chatSaid(h, "You have challenged your target to a duel."));
}

TEST_CASE("the next character is not asked the last one's duel", "[duel][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_DUEL_REQUESTED, Body().u64(kDuelFlag).u64(kThrall));
    h.receive(Opcode::SMSG_DUEL_COUNTDOWN, Body().u32(3000));
    h.pump();
    REQUIRE(h.addonEvents("DUEL_REQUESTED").size() == 1);

    h.relogAs(bob);

    // It was Alice's to answer: accepting would have sent Alice's flag for Bob,
    // and Bob would have been counted into a duel he was never in.
    h.duels().acceptDuel();
    CHECK(h.countSent(Opcode::CMSG_DUEL_ACCEPTED) == 0);
    CHECK(h.duels().getDuelCountdownRemaining() == 0.0f);
}
