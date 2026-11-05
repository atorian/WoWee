// Arena teams: standings, rosters and their order, and the service against a
// recording gateway and the real event bus. Only wowee_arena is linked.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "arena/services/arena_service.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::arena;

namespace {

ArenaTeamMember member(const std::string& name, uint32_t rating, uint8_t classId = 1) {
    ArenaTeamMember m;
    m.name = name;
    m.personalRating = rating;
    m.classId = classId;
    return m;
}

ArenaTeamRoster roster(uint32_t teamId, std::vector<ArenaTeamMember> members) {
    ArenaTeamRoster r;
    r.teamId = teamId;
    r.members = std::move(members);
    return r;
}

std::vector<std::string> names(const ArenaTeamRoster& r) {
    std::vector<std::string> out;
    for (const auto& m : r.members) out.push_back(m.name);
    return out;
}

struct RecordingGateway final : IArenaGateway {
    bool world = true, socket = true;
    std::vector<std::string> sent;

    bool online() const override { return world && socket; }
    bool connected() const override { return socket; }
    void sendAccept() override { sent.push_back("accept"); }
    void sendDecline() override { sent.push_back("decline"); }
    void sendInvite(uint32_t id, const std::string& name) override { sent.push_back("invite " + std::to_string(id) + " " + name); }
    void sendLeave(uint32_t id) override { sent.push_back("leave " + std::to_string(id)); }
    void sendRemove(uint32_t id, const std::string& name) override { sent.push_back("remove " + std::to_string(id) + " " + name); }
    void sendLeader(uint32_t id, const std::string& name) override { sent.push_back("leader " + std::to_string(id) + " " + name); }
    void sendDisband(uint32_t id) override { sent.push_back("disband " + std::to_string(id)); }
    void sendRosterRequest(uint32_t id) override { sent.push_back("roster " + std::to_string(id)); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ArenaService service{gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<TeamsUpdated>([this](const auto&) { events.push_back("teams"); }));
        subscriptions.push_back(bus.subscribe<RosterUpdated>([this](const auto&) { events.push_back("roster"); }));
    }
};

} // namespace

TEST_CASE("Standings keep the name a query gave the team", "[arena]") {
    ArenaTeams t;
    t.named(7, "Gladiators", 2);
    ArenaTeamStats s;
    s.teamId = 7;
    s.rating = 1850;
    t.statsReceived(s);
    REQUIRE(t.stats().size() == 1);
    CHECK(t.stats()[0].teamName == "Gladiators");
    CHECK(t.stats()[0].teamType == 2);
    CHECK(t.stats()[0].rating == 1850);
}

TEST_CASE("A name arriving after the standings fills them in", "[arena]") {
    ArenaTeams t;
    ArenaTeamStats s;
    s.teamId = 7;
    s.rating = 1500;
    t.statsReceived(s);
    s.teamId = 9;
    t.statsReceived(s);
    auto changes = t.named(9, "Second", 3);
    REQUIRE(t.stats().size() == 2);
    CHECK(t.stats()[1].teamName == "Second");
    CHECK(t.stats()[1].rating == 1500);
    CHECK(std::holds_alternative<TeamsUpdated>(changes.front()));
}

TEST_CASE("A roster replaces the team's last one", "[arena]") {
    ArenaTeams t;
    t.rosterReceived(roster(7, {member("A", 1)}));
    t.rosterReceived(roster(9, {member("X", 1)}));
    t.rosterReceived(roster(7, {member("B", 1), member("C", 1)}));
    REQUIRE(t.roster(7));
    CHECK(names(*t.roster(7)) == std::vector<std::string>{"B", "C"});
    CHECK(t.roster(9)->members.size() == 1);
    CHECK_FALSE(t.roster(3));
}

TEST_CASE("A rating sorts from the top, a name from A, and again reverses", "[arena]") {
    ArenaTeams t;
    t.rosterReceived(roster(7, {member("Bob", 1500), member("Ann", 1700), member("Cid", 1600)}));
    t.sortRosters("rating");
    CHECK(names(*t.roster(7)) == std::vector<std::string>{"Ann", "Cid", "Bob"});
    t.sortRosters("rating");
    CHECK(names(*t.roster(7)) == std::vector<std::string>{"Bob", "Cid", "Ann"});
    t.sortRosters("name");
    CHECK(names(*t.roster(7)) == std::vector<std::string>{"Ann", "Bob", "Cid"});
}

TEST_CASE("Ties keep a steady order by name", "[arena]") {
    ArenaTeams t;
    t.rosterReceived(roster(7, {member("Cid", 1500), member("Ann", 1500), member("Bob", 1900)}));
    t.sortRosters("rating");
    CHECK(names(*t.roster(7)) == std::vector<std::string>{"Bob", "Ann", "Cid"});
}

TEST_CASE("A new character has none of the last one's teams, sorted as before", "[arena]") {
    ArenaTeams t;
    t.named(7, "Gladiators", 2);
    t.rosterReceived(roster(7, {member("Bob", 1500), member("Ann", 1700)}));
    t.sortRosters("rating");
    t.beginCharacter();
    CHECK(t.stats().empty());
    CHECK_FALSE(t.roster(7));
    // The column is the interface's: the same one again still reverses.
    t.rosterReceived(roster(9, {member("Bob", 1500), member("Ann", 1700)}));
    t.sortRosters("rating");
    CHECK(names(*t.roster(9)) == std::vector<std::string>{"Bob", "Ann"});
}

TEST_CASE("The roster read back is the one received", "[arena]") {
    Fixture f;
    f.service.onRoster(roster(7, {member("Ann", 1700)}));
    REQUIRE(f.service.getArenaTeamRoster(7));
    CHECK(f.service.getArenaTeamRoster(7)->members.front().name == "Ann");
    CHECK(f.events == std::vector<std::string>{"roster"});
}

TEST_CASE("Commands need a team, and a name where they name someone", "[arena]") {
    Fixture f;
    f.service.arenaTeamInvite(0, "Ann");
    f.service.arenaTeamInvite(7, "");
    f.service.arenaTeamLeave(0);
    f.service.arenaTeamRemove(7, "");
    f.service.arenaTeamSetLeader(0, "Ann");
    f.service.disbandArenaTeam(0);
    CHECK(f.gateway.sent.empty());
    f.service.arenaTeamInvite(7, "Ann");
    f.service.arenaTeamLeave(7);
    f.service.arenaTeamRemove(7, "Bob");
    f.service.arenaTeamSetLeader(7, "Cid");
    f.service.disbandArenaTeam(7);
    CHECK(f.gateway.sent == std::vector<std::string>{"invite 7 Ann", "leave 7", "remove 7 Bob", "leader 7 Cid", "disband 7"});
}

TEST_CASE("A roster may be asked for outside the world, not without a connection", "[arena]") {
    Fixture f;
    f.gateway.world = false;
    f.service.requestArenaTeamRoster(7);
    f.service.acceptArenaTeamInvite();
    CHECK(f.gateway.sent == std::vector<std::string>{"roster 7"});
    f.gateway.socket = false;
    f.service.requestArenaTeamRoster(7);
    CHECK(f.gateway.sent.size() == 1);
}
