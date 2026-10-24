// Arena teams, end to end: the server's team packets, what the interface reads
// and is told, the server's answers in the chat, and the commands that go out.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "arena/services/arena.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kJaina = 0x21;
constexpr uint64_t kThrall = 0x20;

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

/// SMSG_ARENA_TEAM_STATS: the team, its rating, the week's and the season's
/// games and wins, and its rank.
std::vector<uint8_t> teamStats(uint32_t teamId, uint32_t rating) {
    return Body().u32(teamId).u32(rating).u32(10).u32(6).u32(80).u32(50).u32(3).bytes();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

}  // namespace

TEST_CASE("an arena team's roster arrives, is what the interface reads, and sorts", "[arena][harness]") {
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

    const auto* roster = h.arenaTeams().getArenaTeamRoster(7);
    REQUIRE(roster != nullptr);
    REQUIRE(roster->members.size() == 2);
    CHECK(roster->members[0].level == 80);
    CHECK(roster->members[0].personalRating == 1500);

    h.arenaTeams().sortArenaTeamRosters("rating");
    CHECK(h.arenaTeams().getArenaTeamRoster(7)->members.front().name == "Thrall");
    // The same column again reverses it.
    h.arenaTeams().sortArenaTeamRosters("rating");
    CHECK(h.arenaTeams().getArenaTeamRoster(7)->members.front().name == "Jaina");
}

TEST_CASE("a team's name tells the PvP panel, and its standings keep the name", "[arena][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE, Body().u32(7).cstr("Gladiators").u32(2).bytes());
    h.receive(Opcode::SMSG_ARENA_TEAM_STATS, teamStats(7, 1850));
    h.pump();

    CHECK(h.addonEvents("ARENA_TEAM_UPDATE").size() == 1);
    const auto& teams = h.arenaTeams().getArenaTeamStats();
    REQUIRE(teams.size() == 1);
    CHECK(teams[0].teamName == "Gladiators");
    CHECK(teams[0].teamType == 2);
    CHECK(teams[0].rating == 1850);
    CHECK(teams[0].rank == 3);
}

TEST_CASE("a team command's result names the command, who it was about, and why it failed", "[arena][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT, Body().u32(0x01).cstr("Gladiators").cstr("Ann").u32(0).bytes());
    h.receive(Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT, Body().u32(0x03).cstr("Gladiators").cstr("").u32(0x08).bytes());
    h.receive(Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT, Body().u32(0x0E).cstr("").cstr("Bob").u32(0x40).bytes());
    h.pump();

    CHECK(chatSaid(h, "arena team invite succeeded: Gladiators / Ann"));
    CHECK(chatSaid(h, "arena team quit failed (Gladiators): The captain cannot leave; permissions denied."));
    CHECK(chatSaid(h, "arena team captain failed (Bob): The arena team command was refused."));
}

TEST_CASE("an invitation, a team event and a match refusal are said in the chat", "[arena][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_ARENA_TEAM_INVITE, Body().cstr("Thrall").cstr("Gladiators").bytes());
    h.receive(Opcode::SMSG_ARENA_TEAM_EVENT, Body().u8(2).u8(2).cstr("Ann").cstr("Thrall").bytes());
    h.receive(Opcode::SMSG_ARENA_TEAM_EVENT, Body().u8(9).u8(1).cstr("Bob").bytes());
    h.receive(Opcode::SMSG_ARENA_ERROR, Body().u32(2).bytes());
    h.receive(Opcode::SMSG_ARENA_ERROR, Body().u32(9).bytes());
    h.pump();

    CHECK(chatSaid(h, "Thrall has invited you to join Gladiators."));
    CHECK(chatSaid(h, "Ann has been removed from the arena team by Thrall."));
    CHECK(chatSaid(h, "Arena team event 9: Bob"));
    CHECK(chatSaid(h, "That team is full."));
    CHECK(chatSaid(h, "Arena error (code 9)"));
}

TEST_CASE("the team commands name the team, and the player where they name one", "[arena][harness]") {
    GameHarness h;
    h.enterWorld();

    h.arenaTeams().arenaTeamInvite(7, "Ann");
    h.arenaTeams().arenaTeamSetLeader(7, "Bob");
    h.arenaTeams().arenaTeamLeave(7);
    h.arenaTeams().requestArenaTeamRoster(7);

    auto* invite = h.lastSent(Opcode::CMSG_ARENA_TEAM_INVITE);
    REQUIRE(invite);
    CHECK(invite->readUInt32() == 7);
    CHECK(invite->readString() == "Ann");
    auto* leader = h.lastSent(Opcode::CMSG_ARENA_TEAM_LEADER);
    REQUIRE(leader);
    CHECK(leader->readUInt32() == 7);
    CHECK(leader->readString() == "Bob");
    auto* leave = h.lastSent(Opcode::CMSG_ARENA_TEAM_LEAVE);
    REQUIRE(leave);
    CHECK(leave->readUInt32() == 7);
    auto* rosterRequest = h.lastSent(Opcode::CMSG_ARENA_TEAM_ROSTER);
    REQUIRE(rosterRequest);
    CHECK(rosterRequest->readUInt32() == 7);
}

TEST_CASE("the next character has none of the last one's arena teams", "[arena][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE, Body().u32(7).cstr("Gladiators").u32(2).bytes());
    h.receive(Opcode::SMSG_ARENA_TEAM_ROSTER, arenaRoster(7, {{kJaina, "Jaina", 1500}}));
    h.pump();
    REQUIRE(h.arenaTeams().getArenaTeamStats().size() == 1);

    h.relogAs(bob);

    CHECK(h.arenaTeams().getArenaTeamStats().empty());
    CHECK(h.arenaTeams().getArenaTeamRoster(7) == nullptr);
}
