// The group's rules, with nothing but the domain model linked.
#include <catch_amalgamated.hpp>

#include "group/domain/group.hpp"

#include <algorithm>
#include <variant>

using namespace wowee::group;

namespace {

template <typename E>
bool has(const std::vector<GroupChange>& changes) {
    return std::any_of(changes.begin(), changes.end(),
                       [](const GroupChange& c) { return std::holds_alternative<E>(c); });
}

template <typename E>
const E& first(const std::vector<GroupChange>& changes) {
    for (const auto& c : changes)
        if (auto* p = std::get_if<E>(&c)) return *p;
    FAIL("change not reported");
    throw;
}

Member member(uint64_t guid, const std::string& name, bool online = true, uint8_t flags = 0) {
    Member m;
    m.guid = guid;
    m.name = name;
    m.isOnline = online ? 1 : 0;
    m.flags = flags;
    return m;
}

Roster roster(std::vector<Member> members, uint8_t type = 0, uint64_t leader = 0, uint8_t loot = 0) {
    Roster r;
    r.groupType = type;
    r.leaderGuid = leader;
    r.lootMethod = loot;
    r.memberCount = static_cast<uint32_t>(members.size());
    r.members = std::move(members);
    return r;
}

MemberStats health(uint64_t guid, uint32_t hp) {
    MemberStats s;
    s.guid = guid;
    s.updateFlags = StatsField::CurHealth;
    s.present = StatsField::CurHealth;
    s.curHealth = hp;
    return s;
}

} // namespace

TEST_CASE("Members the roster lists online read online before any stats arrive", "[group]") {
    Group g;
    g.rosterReceived(roster({member(1, "Me"), member(2, "Thrall", true), member(3, "Jaina", false)}));
    CHECK((g.member(2)->onlineStatus & 0x1) != 0);
    CHECK((g.member(3)->onlineStatus & 0x1) == 0);
}

TEST_CASE("Joining and leaving are announced once, the loot method only while staying", "[group]") {
    Group g;
    auto joined = g.rosterReceived(roster({member(1, "Me"), member(2, "")}, 0, 1, 3));
    CHECK(first<JoinedGroup>(joined).members == std::vector<std::string>{"Me", "?"});
    CHECK_FALSE(has<LootMethodChanged>(joined));

    auto again = g.rosterReceived(roster({member(1, "Me"), member(2, "Thrall")}, 0, 1, 2));
    CHECK_FALSE(has<JoinedGroup>(again));
    CHECK(first<LootMethodChanged>(again).method == 2);

    CHECK(has<NoLongerInGroup>(g.rosterReceived(roster({}))));
}

TEST_CASE("A WotLK raid is a raid and a dungeon finder group is not", "[group]") {
    Group g;
    CHECK(first<RosterReplaced>(g.rosterReceived(roster({member(1, "Me")}, 0x02))).raid);
    CHECK(first<RosterReplaced>(g.rosterReceived(roster({member(1, "Me")}, 0x01))).raid);
    CHECK_FALSE(first<RosterReplaced>(g.rosterReceived(roster({member(1, "Me")}, 0x0C))).raid);
}

TEST_CASE("An invite that cannot be accepted leaves nothing pending", "[group]") {
    Group g;
    CHECK(has<InviteBlockedAlreadyGrouped>(g.inviteReceived("Thrall", false)));
    CHECK_FALSE(g.hasPendingInvite());
    CHECK(has<InviteReceived>(g.inviteReceived("Thrall", true)));
    CHECK(g.inviterName() == "Thrall");
    g.inviteAnswered();
    CHECK_FALSE(g.hasPendingInvite());
}

TEST_CASE("Member stats apply what the packet carried and report going offline", "[group]") {
    Group g;
    g.rosterReceived(roster({member(1, "Me"), member(2, "Thrall", true)}));
    g.memberStatsReceived(health(2, 900), 1);
    CHECK(g.member(2)->curHealth == 900);
    CHECK(g.member(2)->hasPartyStats);

    MemberStats offline;
    offline.guid = 2;
    offline.updateFlags = StatsField::Status | StatsField::CurHealth;
    offline.present = StatsField::Status;   // the health did not fit
    offline.onlineStatus = 0;
    offline.curHealth = 1;
    const auto changes = g.memberStatsReceived(offline, 1);
    const auto& changed = first<MemberStatsChanged>(changes);
    CHECK(changed.unit == "party1");
    CHECK(changed.onlineChanged);
    CHECK_FALSE(changed.online);
    CHECK(g.member(2)->curHealth == 900);
    CHECK(g.member(2)->isOnline == 0);

    CHECK(g.memberStatsReceived(health(99, 1), 1).empty());
}

TEST_CASE("Unit tokens skip the player in a party and count everyone in a raid", "[group]") {
    Group g;
    g.rosterReceived(roster({member(2, "Thrall"), member(1, "Me"), member(3, "Jaina")}));
    CHECK(g.unitToken(3, 1) == "party2");
    CHECK(g.unitToken(1, 1).empty());
    g.rosterReceived(roster({member(2, "Thrall"), member(1, "Me"), member(3, "Jaina")}, 0x02));
    CHECK(g.unitToken(3, 1) == "raid3");
}

TEST_CASE("A new leader is found by name", "[group]") {
    Group g;
    g.rosterReceived(roster({member(1, "Me"), member(2, "Thrall")}, 0, 1));
    CHECK(first<LeaderChanged>(g.leaderNamed("Thrall")).name == "Thrall");
    CHECK(g.isLeader(2));
}

TEST_CASE("A ready check names the leader when the initiator is unknown", "[group]") {
    Group g;
    g.rosterReceived(roster({member(1, "Me"), member(2, "Thrall")}, 0, 2));
    CHECK(first<ReadyCheckStarted>(g.readyCheckStarted("")).initiator == "Thrall");
    g.readyCheckAnswered(2, "Thrall", false);
    g.readyCheckAnswered(2, "Thrall", true);
    g.readyCheckAnswered(3, "", true);
    REQUIRE(g.readyCheckResults().size() == 1);
    CHECK(g.readyCheckResults()[0].ready);
    const auto finished = g.readyCheckFinished();
    const auto& done = first<ReadyCheckFinished>(finished);
    CHECK(done.ready == 2);
    CHECK(done.notReady == 1);
    CHECK_FALSE(g.hasPendingReadyCheck());
    CHECK(g.readyCheckResults().empty());
}

TEST_CASE("Raid markers: a full list replaces, a single mark moves", "[group]") {
    Group g;
    g.marksReceived(true, {{0, 10}, {7, 11}});
    g.marksReceived(false, {{3, 12}});
    CHECK(g.markGuid(0) == 10);
    CHECK(g.markGuid(3) == 12);
    g.marksReceived(true, {{3, 12}});
    CHECK(g.markGuid(0) == 0);
    CHECK(g.markOf(12) == 3);
    CHECK(g.markOf(0) == Group::kNoMark);
}

TEST_CASE("A local mark is unique per icon and per unit", "[group]") {
    Group g;
    g.markLocally(10, 0);
    g.markLocally(10, 5);
    CHECK(g.markGuid(0) == 0);
    CHECK(g.markOf(10) == 5);
    g.markLocally(11, 5);
    CHECK(g.markOf(10) == Group::kNoMark);
    g.markLocally(11, Group::kNoMark);
    CHECK(g.markGuid(5) == 0);
    CHECK(g.markLocally(11, 9).empty());
}

TEST_CASE("Assignments and the player's flags come from the group", "[group]") {
    Group g;
    g.rosterReceived(roster({member(1, "Me"), member(2, "Thrall", true, MemberFlag::MainTank)}));
    CHECK(g.assigned(Assignment::MainTank) == 2);
    CHECK(g.assigned(Assignment::MainAssist) == 0);
    g.reshaped(0x02, MemberFlag::Assistant, 2, 1);
    CHECK(g.isRaid());
    CHECK(g.isLeader(2));
    CHECK(g.member(1)->flags == MemberFlag::Assistant);
}
