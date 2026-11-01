// Battlegrounds, end to end: the queue statuses, listings, scoreboard and map
// positions the server sends, what the interface reads and is told, the
// server's answers to a join, and the answers that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "battleground/services/battlegrounds.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint32_t kWarsong = 2;
constexpr uint32_t kArathi = 3;
constexpr uint64_t kJaina = 0x21;

/// SMSG_BATTLEFIELD_STATUS for a battleground, as BuildBattlegroundStatusPacket
/// writes it for 3.3.5a: the slot, no arena, the type, the level range, the
/// instance, unrated, then the status and what it carries.
std::vector<uint8_t> status(uint32_t slot, uint32_t bgTypeId, uint32_t statusId) {
    Body b;
    b.u32(slot).u8(0).u8(0).u32(bgTypeId).u8(0x90).u8(0x1F) /* 0x1F90 */.u8(10).u8(19).u32(1).u8(0).u32(statusId);
    if (statusId == 1) b.u32(120000).u32(65000);                    // average wait, time waited
    if (statusId == 2) b.u32(489).u32(0).u32(0).u32(60000);         // map, two unused, invitation
    return b.bytes();
}

/// MSG_PVP_LOG_DATA for a battleground still running, with one row.
std::vector<uint8_t> scoreboard(uint64_t guid, uint32_t damage) {
    return Body()
        .u8(0 /* battleground */).u8(0 /* no winner */).u32(1)
        .u64(guid).u32(3 /* killing blows */).u32(12).u32(1).u32(40)
        .u32(damage).u32(500)
        .u32(2).u32(1).u32(0)                                       // two objective values
        .bytes();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

size_t chatCount(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(),
                                             [&](const auto& line) { return line.message == text; }));
}

std::vector<std::string> uiErrors(GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.addonEvents("UI_ERROR_MESSAGE")) out.push_back(e.args.at(0));
    return out;
}

}  // namespace

TEST_CASE("a queue the server reports is what the interface reads, said once", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    // The third queue, so its number is not its status.
    h.receive(Opcode::SMSG_BATTLEFIELD_STATUS, status(2, kWarsong, 1));
    h.receive(Opcode::SMSG_BATTLEFIELD_STATUS, status(2, kWarsong, 1));
    h.pump();

    const auto& q = h.battlegrounds().getBgQueues()[2];
    CHECK(q.statusId == 1);
    CHECK(q.bgName == "Warsong Gulch");
    CHECK(q.avgWaitTimeSec == 120);
    CHECK(q.minLevel == 10);
    // The server restates a queue every half minute; the line is for a change.
    CHECK(chatCount(h, "Queued for Warsong Gulch.") == 1);
    // Which queue changed, counted from one - for each restatement.
    const auto updates = h.addonEvents("UPDATE_BATTLEFIELD_STATUS");
    REQUIRE(updates.size() == 2);
    CHECK(updates[0].args == std::vector<std::string>{"3"});
}

TEST_CASE("an invitation is accepted for the queue it came to", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_BATTLEFIELD_STATUS, status(1, kWarsong, 2));
    h.pump();
    CHECK(chatSaid(h, "Warsong Gulch is ready!"));
    REQUIRE(h.battlegrounds().hasPendingBgInvite());
    CHECK(h.battlegrounds().getBgQueues()[1].inviteTimeout == 60);

    h.battlegrounds().acceptBattlefield(1);

    auto* port = h.lastSent(Opcode::CMSG_BATTLEFIELD_PORT);
    REQUIRE(port);
    port->readUInt8();                          // arena type
    port->readUInt8();
    CHECK(port->readUInt32() == kWarsong);
    port->readUInt16();
    CHECK(port->readUInt8() == 1);              // going in
    CHECK(chatSaid(h, "Accepting battleground invitation..."));
    CHECK_FALSE(h.battlegrounds().hasPendingBgInvite());
}

TEST_CASE("leaving a queue still waiting refuses it and empties the slot", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_BATTLEFIELD_STATUS, status(0, kWarsong, 1));
    h.pump();

    h.battlegrounds().declineBattlefield(0);
    h.pump();

    auto* port = h.lastSent(Opcode::CMSG_BATTLEFIELD_PORT);
    REQUIRE(port);
    port->readUInt8();
    port->readUInt8();
    CHECK(port->readUInt32() == kWarsong);
    port->readUInt16();
    CHECK(port->readUInt8() == 0);              // turned down
    CHECK(chatSaid(h, "Left the queue for Warsong Gulch."));
    CHECK(h.battlegrounds().getBgQueues()[0].statusId == 0);
    CHECK(h.addonEvents("UPDATE_BATTLEFIELD_STATUS").size() == 2);
}

TEST_CASE("answering with no invitation says so and sends nothing", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    h.battlegrounds().acceptBattlefield();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_BATTLEFIELD_PORT));
    CHECK(chatSaid(h, "No battleground invitation pending."));
}

TEST_CASE("the server's answer to a join says what it was", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(kJaina).u8(0).cstr("Jaina").cstr("").u8(1).u8(1).u8(8).bytes());
    h.pump();

    h.receive(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, Body().u32(static_cast<uint32_t>(-2)).bytes());
    h.receive(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, Body().u32(static_cast<uint32_t>(-11)).u64(kJaina).bytes());
    h.receive(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, Body().u32(static_cast<uint32_t>(-99)).bytes());
    h.receive(Opcode::SMSG_GROUP_JOINED_BATTLEGROUND, Body().u32(kArathi).bytes());
    h.pump();

    const std::string deserter =
        "You cannot join the battleground yet because you or one of your party members is flagged as a Deserter.";
    CHECK(uiErrors(h) == std::vector<std::string>{deserter, "Jaina was unavailable to join the queue.",
                                                  "Could not join the battleground queue (-99)."});
    CHECK(chatSaid(h, deserter));
    CHECK(chatSaid(h, "Your group has joined the queue for Arathi Basin"));
}

TEST_CASE("a join the server never answers is said after five seconds", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    h.battlegrounds().joinBattlefield(0, kArathi, 0, false);
    auto* join = h.lastSent(Opcode::CMSG_BATTLEMASTER_JOIN);
    REQUIRE(join);
    join->readUInt64();
    CHECK(join->readUInt32() == kArathi);

    h.advance(3.0f);
    CHECK(uiErrors(h).empty());
    h.advance(3.0f);
    CHECK(uiErrors(h) == std::vector<std::string>{
        "The server did not answer the request to queue for Arathi Basin - this realm may not offer it to you."});
}

TEST_CASE("a join the server answers is not said to be unanswered", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    h.battlegrounds().joinBattlefield(0, kArathi, 0, false);
    h.receive(Opcode::SMSG_BATTLEFIELD_STATUS, status(0, kArathi, 1));
    h.pump();
    h.advance(6.0f);

    CHECK(uiErrors(h).empty());
}

TEST_CASE("the scoreboard arrives, is read, and a cut-short one leaves it", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::MSG_PVP_LOG_DATA, scoreboard(kJaina, 9000));
    h.pump();

    CHECK(h.addonEvents("UPDATE_BATTLEFIELD_SCORE").size() == 1);
    const auto* board = h.battlegrounds().getBgScoreboard();
    REQUIRE(board);
    REQUIRE(board->players.size() == 1);
    CHECK(board->players[0].killingBlows == 3);
    CHECK(board->players[0].damageDone == 9000);
    CHECK(board->players[0].bgStats.size() == 2);

    // Cut short before the player list: the scoreboard on screen stays.
    h.receive(Opcode::MSG_PVP_LOG_DATA, Body().u8(0).u8(0).bytes());
    h.pump();
    CHECK(h.addonEvents("UPDATE_BATTLEFIELD_SCORE").size() == 1);
    REQUIRE(h.battlegrounds().getBgScoreboard());
    CHECK(h.battlegrounds().getBgScoreboard()->players[0].damageDone == 9000);
}

TEST_CASE("the map's positions are both teams' lists", "[battleground][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS,
              Body().u32(0).u32(1).u64(kJaina).f32(100.0f).f32(-50.0f).bytes());
    h.pump();

    const auto& positions = h.battlegrounds().getBgPlayerPositions();
    REQUIRE(positions.size() == 1);
    CHECK(positions[0].guid == kJaina);
    CHECK(positions[0].group == 1);
    CHECK(positions[0].wowX == 100.0f);
}

TEST_CASE("the next character has none of the last one's battlegrounds", "[battleground][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_BATTLEFIELD_STATUS, status(0, kWarsong, 2));
    h.receive(Opcode::MSG_PVP_LOG_DATA, scoreboard(kJaina, 9000));
    h.pump();
    REQUIRE(h.battlegrounds().hasPendingBgInvite());

    h.relogAs(bob);

    CHECK_FALSE(h.battlegrounds().hasPendingBgInvite());
    CHECK(h.battlegrounds().getBgScoreboard() == nullptr);
    h.battlegrounds().acceptBattlefield();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_BATTLEFIELD_PORT));
}
