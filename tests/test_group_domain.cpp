// The group, end to end: the server's party and raid packets, what the
// interface reads and is told, the lines the player sees, and the requests
// that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "group/domain/events.hpp"
#include "group/services/group.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kJaina = 0x21;
constexpr uint64_t kThrall = 0x22;
constexpr uint64_t kMurloc = 0xF130000000000123ull;

/// SMSG_GROUP_LIST, as AzerothCore's Group::SendUpdate writes it: the type,
/// the player's subgroup, flags and roles, the group, then the others (each
/// online, in subgroup 0), the leader and the loot settings.
std::vector<uint8_t> groupList(uint8_t groupType, uint64_t leader,
                               const std::vector<std::pair<std::string, uint64_t>>& others,
                               uint8_t lootMethod = 3, uint8_t memberFlags = 0) {
    Body b;
    b.u8(groupType).u8(0).u8(0).u8(0);
    b.u64(0x1234).u32(1).u32(static_cast<uint32_t>(others.size()));
    for (const auto& [name, guid] : others) {
        b.cstr(name).u64(guid).u8(1 /* online */).u8(0).u8(memberFlags).u8(0);
    }
    b.u64(leader);
    b.u8(lootMethod).u64(leader).u8(2).u8(1).u8(0).u8(0);
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

void knowName(GameHarness& h, uint64_t guid, const std::string& name) {
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(guid).u8(0).cstr(name).cstr("").u8(1).u8(1).u8(8).bytes());
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

std::vector<std::string> uiErrors(GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.addonEvents("UI_ERROR_MESSAGE")) out.push_back(e.args.at(0));
    return out;
}

}  // namespace

TEST_CASE("the party the server lists is what the interface reads, and joining is said once",
          "[group][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}, 3));
    h.pump();

    REQUIRE(h.group().isInGroup());
    const auto& party = h.group().getPartyData();
    CHECK(party.leaderGuid == kJaina);
    REQUIRE(party.members.size() == 1);
    CHECK(party.members[0].name == "Jaina");
    CHECK(h.group().unitTokenOf(kJaina) == "party1");
    CHECK(chatSaid(h, "You joined a group with: Jaina"));
    CHECK(h.addonEvents("GROUP_ROSTER_UPDATE").size() == 1);
    CHECK(h.addonEvents("RAID_ROSTER_UPDATE").empty());

    // The same group with another loot method: said, and not joined again.
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}, 2));
    h.pump();
    CHECK(chatSaid(h, "Loot method changed to Master Looter."));
    CHECK(h.addonEvents("PARTY_LOOT_METHOD_CHANGED").size() == 1);
    const auto& history = h.game().chat().getChatHistory();
    CHECK(std::count_if(history.begin(), history.end(), [](const auto& line) {
              return line.message == "You joined a group with: Jaina";
          }) == 1);

    // An empty list is the group gone.
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, 0, {}));
    h.pump();
    CHECK_FALSE(h.group().isInGroup());
    CHECK(chatSaid(h, "You are no longer in a group."));
}

TEST_CASE("a group list cut short leaves the group as it was", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}));
    h.pump();
    const size_t rosterUpdates = h.addonEvents("GROUP_ROSTER_UPDATE").size();

    // The list ends after the group's guid, before the update counter and
    // the member count.
    h.receive(Opcode::SMSG_GROUP_LIST, Body().u8(0).u8(0).u8(0).u8(0).u64(0x1234).bytes());
    h.pump();

    REQUIRE(h.group().isInGroup());
    CHECK(h.group().getPartyData().members.size() == 1);
    CHECK(h.addonEvents("GROUP_ROSTER_UPDATE").size() == rosterUpdates);
    CHECK_FALSE(chatSaid(h, "You are no longer in a group."));
}

TEST_CASE("a raid list is a raid, and members are counted from the first raid slot", "[group][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0x02, kJaina, {{"Jaina", kJaina}, {"Thrall", kThrall}}));
    h.pump();

    CHECK(h.addonEvents("RAID_ROSTER_UPDATE").size() == 1);
    CHECK(h.group().unitTokenOf(kThrall) == "raid2");
}

TEST_CASE("a party member's auras from the stats packet are the ones the frames read", "[group][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.receive(Opcode::SMSG_PARTY_MEMBER_STATS, memberAuras(kJaina, {{0, 1459}, {3, 10938}}));
    h.pump();

    // The party frames ask GameHandler::getUnitAuras, which is SpellHandler's
    // cache.
    const auto* auras = h.game().spells().getUnitAuras(kJaina);
    REQUIRE(auras != nullptr);
    REQUIRE(auras->size() == 2);
    CHECK((*auras)[0].spellId == 1459);
    CHECK((*auras)[1].spellId == 10938);
    const auto auraEvents = h.addonEvents("UNIT_AURA");
    REQUIRE(auraEvents.size() == 1);
    CHECK(auraEvents[0].args == std::vector<std::string>{"party1"});
}

TEST_CASE("a member's health and going offline reach the frames by the member's unit", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}, {"Thrall", kThrall}}));
    h.pump();

    // Health only, as a 3.3.5a uint32.
    h.receive(Opcode::SMSG_PARTY_MEMBER_STATS, Body().packedGuid(kThrall).u32(0x0002).u32(4321).bytes());
    // The status alone, offline.
    h.receive(Opcode::SMSG_PARTY_MEMBER_STATS, Body().packedGuid(kJaina).u32(0x0001).u8(0).u8(0).bytes());
    h.pump();

    const auto health = h.addonEvents("UNIT_HEALTH");
    REQUIRE(health.size() == 1);
    CHECK(health[0].args == std::vector<std::string>{"party2"});
    const auto& members = h.group().getPartyData().members;
    const auto thrall = std::find_if(members.begin(), members.end(), [](const auto& m) { return m.guid == kThrall; });
    REQUIRE(thrall != members.end());
    CHECK(thrall->curHealth == 4321);
    CHECK(h.addonEvents("PARTY_MEMBER_DISABLE").size() == 1);
}

TEST_CASE("stats for someone not in the group are not applied", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.pump();

    h.receive(Opcode::SMSG_PARTY_MEMBER_STATS, memberAuras(kThrall, {{0, 1459}}));
    h.pump();

    CHECK(h.addonEvents("UNIT_AURA").empty());
    const auto* auras = h.game().spells().getUnitAuras(kThrall);
    CHECK((auras == nullptr || auras->empty()));
}

TEST_CASE("a group invitation is offered, and accepting it answers the server", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<std::string> invitedBy;
    auto sub = h.events().subscribe<wowee::group::InviteReceived>(
        [&](const wowee::group::InviteReceived& e) { invitedBy.push_back(e.inviter); });

    h.receive(Opcode::SMSG_GROUP_INVITE, Body().u8(1).cstr("Jaina"));
    h.pump();

    const auto offered = h.addonEvents("PARTY_INVITE_REQUEST");
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].args.at(0) == "Jaina");
    CHECK(invitedBy == std::vector<std::string>{"Jaina"});
    CHECK(h.group().getPendingInviterName() == "Jaina");

    h.group().acceptGroupInvite();
    CHECK(h.countSent(Opcode::CMSG_GROUP_ACCEPT) == 1);
    CHECK_FALSE(h.group().hasPendingGroupInvite());
}

TEST_CASE("an invitation that could not be offered is not offered", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    size_t invites = 0;
    auto sub = h.events().subscribe<wowee::group::InviteReceived>(
        [&](const wowee::group::InviteReceived&) { ++invites; });

    // A zero in front: the inviter tried, and the server could not offer it
    // because the player is already in a group.
    h.receive(Opcode::SMSG_GROUP_INVITE, Body().u8(0).cstr("Jaina"));
    h.pump();

    CHECK(h.addonEvents("PARTY_INVITE_REQUEST").empty());
    CHECK(invites == 0);
    CHECK_FALSE(h.group().hasPendingGroupInvite());
}

TEST_CASE("a refused party command tells the player why, in the server's terms", "[group][harness]") {
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
        CHECK(uiErrors(h) == std::vector<std::string>{c.said});
    }

    // Success says nothing.
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PARTY_COMMAND_RESULT, Body().u32(0).cstr("Jaina").u32(0));
    h.pump();
    CHECK(h.addonEvents("UI_ERROR_MESSAGE").empty());
}

TEST_CASE("a disbanded party and being removed both leave the group, said on the error line",
          "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}));
    h.pump();

    h.receive(Opcode::SMSG_GROUP_DESTROYED, Body().bytes());
    h.pump();
    CHECK_FALSE(h.group().isInGroup());

    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}));
    h.receive(Opcode::SMSG_GROUP_UNINVITE, Body().bytes());
    h.pump();
    CHECK_FALSE(h.group().isInGroup());

    CHECK(uiErrors(h) == std::vector<std::string>{"Your party has been disbanded.",
                                                  "You have been removed from the group."});
}

TEST_CASE("a new leader by name is said and the frames are told", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.pump();

    h.receive(Opcode::SMSG_GROUP_SET_LEADER, Body().cstr("Jaina").bytes());
    h.pump();

    CHECK(h.group().getPartyData().leaderGuid == kJaina);
    CHECK(chatSaid(h, "Jaina is now the group leader."));
    CHECK(h.addonEvents("PARTY_LEADER_CHANGED").size() == 1);
}

TEST_CASE("a ready check names the leader, records the answers and is finished by the server",
          "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    knowName(h, kJaina, "Jaina");
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}));
    h.pump();

    // Started by a unit out of view: the leader started it.
    h.receive(Opcode::MSG_RAID_READY_CHECK, Body().u64(kJaina).bytes());
    h.pump();
    REQUIRE(h.group().hasPendingReadyCheck());
    CHECK(chatSaid(h, "Jaina initiated a ready check!"));
    CHECK(h.addonEvents("READY_CHECK").at(0).args == std::vector<std::string>{"Jaina"});

    h.group().respondToReadyCheck(true);
    auto* answer = h.lastSent(Opcode::MSG_RAID_READY_CHECK);
    REQUIRE(answer);
    CHECK(answer->readUInt8() == 1);

    h.receive(Opcode::MSG_RAID_READY_CHECK_CONFIRM, Body().u64(kJaina).u8(1).bytes());
    h.pump();
    CHECK(chatSaid(h, "Jaina is Ready."));
    CHECK(h.addonEvents("READY_CHECK_CONFIRM").at(0).args ==
          std::vector<std::string>{"0x0000000000000021", "1"});
    REQUIRE(h.group().getReadyCheckResults().size() == 1);
    CHECK(h.group().getReadyCheckResults()[0].ready);

    h.receive(Opcode::MSG_RAID_READY_CHECK_FINISHED, Body().bytes());
    h.pump();
    CHECK_FALSE(h.group().hasPendingReadyCheck());
    CHECK(chatSaid(h, "Ready check complete: 1 ready, 0 not ready."));
    CHECK(h.addonEvents("READY_CHECK_FINISHED").size() == 1);
}

TEST_CASE("a ready check alone is refused, in a group it goes to the server", "[group][harness]") {
    GameHarness h;
    h.enterWorld();

    h.group().initiateReadyCheck();
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::MSG_RAID_READY_CHECK));
    CHECK(uiErrors(h) == std::vector<std::string>{"You must be in a group to initiate a ready check."});

    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.pump();
    h.group().initiateReadyCheck();
    CHECK(h.lastSent(Opcode::MSG_RAID_READY_CHECK));
    CHECK(chatSaid(h, "Ready check initiated."));
}

TEST_CASE("a solo player's raid mark stays on this client, a grouped one goes to the server",
          "[group][harness]") {
    GameHarness h;
    h.enterWorld();

    h.group().setRaidMark(kMurloc, 7);
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::MSG_RAID_TARGET_UPDATE));
    CHECK(h.group().getRaidMarkGuid(7) == kMurloc);
    CHECK(h.group().getEntityRaidMark(kMurloc) == 7);
    CHECK(h.addonEvents("RAID_TARGET_UPDATE").size() == 1);

    // In a group the server's full list replaces the local marks.
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.receive(Opcode::MSG_RAID_TARGET_UPDATE, Body().u8(1).u8(2).u64(kJaina).bytes());
    h.pump();
    CHECK(h.group().getRaidMarkGuid(7) == 0);
    CHECK(h.group().getRaidMarkGuid(2) == kJaina);

    h.group().setRaidMark(kMurloc, 5);
    auto* mark = h.lastSent(Opcode::MSG_RAID_TARGET_UPDATE);
    REQUIRE(mark);
    CHECK(mark->readUInt8() == 5);
    CHECK(mark->readUInt64() == kMurloc);
    // Nothing changes until the server says so.
    CHECK(h.group().getRaidMarkGuid(5) == 0);

    // Clearing a mark names the icon it holds and no one.
    h.group().setRaidMark(kJaina, 0xFF);
    mark = h.lastSent(Opcode::MSG_RAID_TARGET_UPDATE);
    REQUIRE(mark);
    CHECK(mark->readUInt8() == 2);
    CHECK(mark->readUInt64() == 0);
}

TEST_CASE("one mark moved by the server leaves the others", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.receive(Opcode::MSG_RAID_TARGET_UPDATE, Body().u8(1).u8(0).u64(kJaina).u8(1).u64(kThrall).bytes());
    // A single mark: who placed it, then the icon and its unit.
    h.receive(Opcode::MSG_RAID_TARGET_UPDATE, Body().u8(0).u64(kJaina).u8(4).u64(kMurloc).bytes());
    h.pump();

    CHECK(h.group().getRaidMarkGuid(0) == kJaina);
    CHECK(h.group().getRaidMarkGuid(1) == kThrall);
    CHECK(h.group().getRaidMarkGuid(4) == kMurloc);
}

TEST_CASE("the world the server places the player in starts with no raid marks", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.receive(Opcode::MSG_RAID_TARGET_UPDATE, Body().u8(1).u8(2).u64(kMurloc).bytes());
    h.pump();
    REQUIRE(h.group().getRaidMarkGuid(2) == kMurloc);

    // A late LOGIN_VERIFY_WORLD for the same spot is a duplicate and ignored;
    // one for another map places the player anew.
    PlayerSpec elsewhere = h.player();
    elsewhere.mapId = 1;
    h.receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, GameHarness::loginVerifyWorldBody(elsewhere));
    h.pump();

    CHECK(h.group().getRaidMarkGuid(2) == 0);
    CHECK(h.group().isInGroup());
}

TEST_CASE("the main tank is a party assignment, taken from whoever holds it", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    // Thrall holds the main tank (member flag 0x02).
    h.receive(Opcode::SMSG_GROUP_LIST,
              groupList(0x02, h.player().guid, {{"Thrall", kThrall}}, 3, 0x02));
    h.pump();

    h.group().setMainTank(kJaina);
    h.pump();
    auto* assign = h.lastSent(Opcode::MSG_PARTY_ASSIGNMENT);
    REQUIRE(assign);
    CHECK(assign->readUInt8() == 0);            // main tank
    CHECK(assign->readUInt8() == 1);            // given
    CHECK(assign->readUInt64() == kJaina);
    CHECK_FALSE(h.lastSent(Opcode::MSG_RAID_TARGET_UPDATE));
    CHECK(chatSaid(h, "Main tank set."));

    h.group().clearMainTank();
    h.pump();
    assign = h.lastSent(Opcode::MSG_PARTY_ASSIGNMENT);
    REQUIRE(assign);
    CHECK(assign->readUInt8() == 0);
    CHECK(assign->readUInt8() == 0);            // taken away
    CHECK(assign->readUInt64() == kThrall);
    CHECK(chatSaid(h, "Main tank cleared."));

    h.group().setMainAssist(0);
    h.pump();
    CHECK(uiErrors(h) == std::vector<std::string>{"You must have a target selected."});
}

TEST_CASE("the loot request names the master looter only for master loot", "[group][harness]") {
    GameHarness h;
    h.enterWorld();

    h.group().setLootMethod(2, kJaina, 3);
    auto* loot = h.lastSent(Opcode::CMSG_LOOT_METHOD);
    REQUIRE(loot);
    CHECK(loot->readUInt32() == 2);
    CHECK(loot->readUInt64() == kJaina);
    CHECK(loot->readUInt32() == 3);

    h.group().setLootMethod(3, kJaina, 2);
    loot = h.lastSent(Opcode::CMSG_LOOT_METHOD);
    REQUIRE(loot);
    CHECK(loot->readUInt32() == 3);
    CHECK(loot->readUInt64() == 0);
    CHECK(loot->readUInt32() == 2);
}

TEST_CASE("only the leader of a party converts it to a raid", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}));
    h.pump();

    h.group().convertToRaid();
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GROUP_RAID_CONVERT));
    CHECK(uiErrors(h) == std::vector<std::string>{"You must be the party leader to convert to raid."});

    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.pump();
    h.group().convertToRaid();
    CHECK(h.lastSent(Opcode::CMSG_GROUP_RAID_CONVERT));
}

TEST_CASE("removing a member needs a name, and leaving is at once", "[group][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, h.player().guid, {{"Jaina", kJaina}}));
    h.pump();

    h.group().uninvitePlayer("");
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GROUP_UNINVITE));
    CHECK(uiErrors(h) == std::vector<std::string>{"You must specify a player name to uninvite."});

    h.group().uninvitePlayer("Jaina");
    auto* uninvite = h.lastSent(Opcode::CMSG_GROUP_UNINVITE);
    REQUIRE(uninvite);
    CHECK(uninvite->readString() == "Jaina");
    CHECK(chatSaid(h, "Removed Jaina from the group."));

    h.group().leaveGroup();
    h.pump();
    CHECK(h.lastSent(Opcode::CMSG_GROUP_DISBAND));
    CHECK_FALSE(h.group().isInGroup());
}

TEST_CASE("the next character has none of the last one's group", "[group][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_GROUP_LIST, groupList(0, kJaina, {{"Jaina", kJaina}}));
    // SMSG_GROUP_INVITE, 3.3.5a: whether it can be accepted, then who asks.
    h.receive(Opcode::SMSG_GROUP_INVITE, Body().u8(1).cstr("Thrall").u32(0).u8(0).u32(0));
    h.receive(Opcode::MSG_RAID_READY_CHECK, Body().u64(kJaina).bytes());
    h.receive(Opcode::MSG_RAID_TARGET_UPDATE, Body().u8(1).u8(2).u64(kMurloc).bytes());
    h.pump();
    REQUIRE(h.group().isInGroup());
    REQUIRE(h.group().hasPendingGroupInvite());

    h.relogAs(bob);

    // It was Alice's to answer: Bob was asked to join a group nobody had
    // offered him.
    CHECK_FALSE(h.group().hasPendingGroupInvite());
    CHECK_FALSE(h.group().isInGroup());
    CHECK_FALSE(h.group().hasPendingReadyCheck());
    CHECK(h.group().getRaidMarkGuid(2) == 0);
}
