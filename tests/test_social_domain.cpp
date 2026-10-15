// Groups, duels and arena teams, end to end: the server's packets, what the
// interface reads and is told, and what the game answers with.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_events.hpp"
#include "game/world_packet_data.hpp"

#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

namespace {

constexpr uint64_t kJaina = 0x21;
constexpr uint64_t kThrall = 0x20;
constexpr uint64_t kDuelFlag = 0xF110000000000099ull;

/// SMSG_GROUP_LIST for a plain party of the player and others, as AzerothCore's
/// Group::SendUpdate writes it.
std::vector<uint8_t> party(uint64_t leader, const std::vector<std::pair<std::string, uint64_t>>& others) {
    Body b;
    b.u8(0 /* party */).u8(0).u8(0).u8(0);
    b.u64(0x1234).u32(1).u32(static_cast<uint32_t>(others.size()));
    for (const auto& [name, guid] : others) {
        b.cstr(name).u64(guid).u8(1 /* online */).u8(0).u8(0).u8(0);
    }
    b.u64(leader);
    b.u8(2).u64(leader).u8(2).u8(1).u8(0).u8(0);  // loot method, master, threshold, difficulties
    return b.bytes();
}

/// SMSG_PARTY_MEMBER_STATS carrying only a member's auras: the aura mask, then
/// a spell and its flags for each set bit.
std::vector<uint8_t> memberAuras(uint64_t member, const std::vector<std::pair<int, uint32_t>>& slots) {
    Body b;
    b.packedGuid(member).u32(0x0200);
    uint64_t mask = 0;
    for (const auto& [slot, spell] : slots) mask |= (uint64_t(1) << slot);
    b.u64(mask);
    for (const auto& [slot, spell] : slots) b.u32(spell).u8(0);
    return b.bytes();
}

struct RosterMember {
    uint64_t guid;
    std::string name;
    uint32_t rating;
};

/// SMSG_ARENA_TEAM_ROSTER: the team, no trailing floats, the members.
std::vector<uint8_t> arenaRoster(uint32_t teamId, const std::vector<RosterMember>& members) {
    Body b;
    b.u32(teamId).u8(0).u32(static_cast<uint32_t>(members.size())).u32(2 /* 2v2 */);
    for (const auto& m : members) {
        b.u64(m.guid).u8(1).cstr(m.name).u32(1 /* member */).u8(80).u8(1);
        b.u32(10).u32(5).u32(40).u32(20).u32(m.rating);
    }
    return b.bytes();
}

}  // namespace

TEST_CASE("a party member's auras from the stats packet are the ones the frames read",
          "[social][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GROUP_LIST, party(h.player().guid, {{"Jaina", kJaina}}));
    h.receive(Opcode::SMSG_PARTY_MEMBER_STATS, memberAuras(kJaina, {{0, 1459}, {3, 10938}}));
    h.pump();

    // The party frames ask GameHandler::getUnitAuras, which is SpellHandler's
    // cache. This packet wrote a GameHandler copy of it instead, so a member's
    // auras from it were never shown.
    const auto* auras = h.game().getUnitAuras(kJaina);
    REQUIRE(auras != nullptr);
    REQUIRE(auras->size() == 2);
    CHECK((*auras)[0].spellId == 1459);
    CHECK((*auras)[1].spellId == 10938);
}

TEST_CASE("an arena team's roster arrives, is what the interface reads, and sorts",
          "[social][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_ARENA_TEAM_ROSTER,
              arenaRoster(7, {{kJaina, "Jaina", 1500}, {kThrall, "Thrall", 1800}}));
    h.pump();

    // Fired with no arguments: pvpframe reads one as "ask for the roster
    // again", so any argument would answer a roster with another request.
    const auto updates = h.addonEvents("ARENA_TEAM_ROSTER_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args.empty());

    // The Lua roster and its sort used a GameHandler copy nothing filled.
    const auto* roster = h.game().getArenaTeamRoster(7);
    REQUIRE(roster != nullptr);
    REQUIRE(roster->members.size() == 2);

    h.game().sortArenaTeamRosters("rating");
    CHECK(h.game().getArenaTeamRoster(7)->members.front().name == "Thrall");
    // The same column again reverses it.
    h.game().sortArenaTeamRosters("rating");
    CHECK(h.game().getArenaTeamRoster(7)->members.front().name == "Jaina");
}

TEST_CASE("a group invitation is offered, and accepting it answers the server", "[social][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<std::string> invitedBy;
    auto sub = h.events().subscribe<ev::GroupInviteReceived>(
        [&](const ev::GroupInviteReceived& e) { invitedBy.push_back(e.inviterName); });

    h.receive(Opcode::SMSG_GROUP_INVITE, Body().u8(1).cstr("Jaina"));
    h.pump();

    const auto offered = h.addonEvents("PARTY_INVITE_REQUEST");
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].args.at(0) == "Jaina");
    CHECK(invitedBy == std::vector<std::string>{"Jaina"});

    h.game().acceptGroupInvite();
    CHECK(h.countSent(Opcode::CMSG_GROUP_ACCEPT) == 1);
}

TEST_CASE("an invitation that could not be offered is not offered", "[social][harness]") {
    GameHarness h;
    h.enterWorld();
    size_t invites = 0;
    auto sub = h.events().subscribe<ev::GroupInviteReceived>([&](const ev::GroupInviteReceived&) { ++invites; });

    // A zero in front: the inviter tried, and the server could not offer it
    // because the player is already in a group.
    h.receive(Opcode::SMSG_GROUP_INVITE, Body().u8(0).cstr("Jaina"));
    h.pump();

    CHECK(h.addonEvents("PARTY_INVITE_REQUEST").empty());
    CHECK(invites == 0);
}

TEST_CASE("a refused party command tells the player why, in the server's terms",
          "[social][harness]") {
    // SMSG_PARTY_COMMAND_RESULT: the operation, the name it was tried on, and
    // the result as AzerothCore numbers it (PartyResult in SharedDefines.h).
    // The codes step from 9 to 12, so a table off by those two reads every
    // later refusal as the wrong one.
    struct Case {
        uint32_t result;
        const char* said;
    };
    const Case cases[] = {
        {1, "No player named \"Jaina\" is currently online."},
        {7, "You are not the group leader."},
        {9, "Jaina is ignoring you."},
        {12, "You cannot do that while in a LFG queue."},
        {13, "Target is not accepting group invites."},
        {200, "Party command failed."},
    };
    for (const auto& c : cases) {
        INFO("result " << c.result);
        GameHarness h;
        h.enterWorld();
        h.receive(Opcode::SMSG_PARTY_COMMAND_RESULT, Body().u32(0 /* invite */).cstr("Jaina").u32(c.result));
        h.pump();
        const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].args.at(0) == c.said);
    }

    // Success says nothing.
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PARTY_COMMAND_RESULT, Body().u32(0).cstr("Jaina").u32(0));
    h.pump();
    CHECK(h.addonEvents("UI_ERROR_MESSAGE").empty());
}

TEST_CASE("the guild bank's balance event is the bank's figure, not a chat line",
          "[social][harness]") {
    GameHarness h;
    h.enterWorld();
    const size_t linesBefore = h.game().getChatHistory().size();

    // Type 17, one string: the balance as sixteen zero-padded hex digits,
    // broadcast to every member whenever anyone touches the bank. Unhandled,
    // it reached guild chat as "Guild event 17: 000000000051451B".
    h.receive(Opcode::SMSG_GUILD_EVENT, Body().u8(17).u8(1).cstr("000000000051451B"));
    h.pump();

    CHECK(h.game().getChatHistory().size() == linesBefore);
    CHECK(h.addonEvents("CHAT_MSG_SYSTEM").empty());
    CHECK(h.addonEvents("CHAT_MSG_GUILD").empty());
    // 532g 61s 7c, read as hex.
    CHECK(h.game().getGuildBankData().money == 5326107u);
}

TEST_CASE("a duel challenge names the challenger, and accepting names the flag", "[social][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(kThrall).u8(0).cstr("Thrall").cstr("").u8(2).u8(0).u8(7));
    h.pump();

    h.receive(Opcode::SMSG_DUEL_REQUESTED, Body().u64(kDuelFlag).u64(kThrall));
    h.pump();

    const auto requested = h.addonEvents("DUEL_REQUESTED");
    REQUIRE(requested.size() == 1);
    CHECK(requested[0].args.at(0) == "Thrall");

    h.game().acceptDuel();
    auto* accepted = h.lastSent(Opcode::CMSG_DUEL_ACCEPTED);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->readUInt64() == kDuelFlag);
}
