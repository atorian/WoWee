// Reputation: where a standing starts, how standings move, the rows the
// reputation panel draws, and the service against a fake Faction.dbc, a
// recording gateway and the real event bus. Only wowee_reputation is linked.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "reputation/services/reputation_service.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::reputation;

namespace {

FactionRecord faction(uint32_t id, const std::string& name, uint32_t index, uint32_t parent = 0) {
    FactionRecord r;
    r.id = id;
    r.name = name;
    r.reputationIndex = index;
    r.parentId = parent;
    return r;
}

// Classic (no standing) holds Alliance (standing), which holds Stormwind and
// Darnassus; Kurenai stands alone.
std::vector<FactionRecord> world() {
    return {
        faction(1118, "Classic", kNoReputationIndex),
        faction(469, "Alliance", 11, 1118),
        faction(72, "Stormwind", 1, 469),
        faction(69, "Darnassus", 2, 469),
        faction(978, "Kurenai", 3),
    };
}

std::vector<FactionStanding> metAll(size_t n = 12) {
    std::vector<FactionStanding> s(n);
    for (auto& f : s) f.flags = kFactionVisible;
    return s;
}

std::vector<std::string> names(const std::vector<ReputationRow>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.name);
    return out;
}

struct FakeDbc final : IFactionData {
    bool assets = true;
    std::vector<FactionRecord> rows = world();
    bool ready() const override { return assets; }
    std::vector<FactionRecord> factions() override { return rows; }
};

struct RecordingGateway final : IReputationGateway {
    bool world = true;
    uint8_t race = 1, cls = 1;
    std::vector<std::string> sent;
    bool online() const override { return world; }
    uint8_t playerRace() const override { return race; }
    uint8_t playerClass() const override { return cls; }
    void sendWatched(int32_t i) override { sent.push_back("watch " + std::to_string(i)); }
    void sendAtWar(uint32_t i, bool w) override { sent.push_back("war " + std::to_string(i) + (w ? " on" : " off")); }
    void sendInactive(uint32_t i, bool v) override { sent.push_back("inactive " + std::to_string(i) + (v ? " on" : " off")); }
};

} // namespace

TEST_CASE("A standing starts where the slot for the player's race and class says", "[reputation]") {
    FactionRecord kurenai = faction(978, "Kurenai", 3);
    kurenai.hasBase = true;
    // Slot 0: draenei (race 11) of any class start at 0; slot 1: every other
    // Alliance race (humans, race 1) start at -1200; slot 2: class-only, rogues
    // (class 4) start at 500.
    kurenai.base = {1 << 10, 1 << 0, 0, 0,
                    0, 0, 1 << 3, 0,
                    0, -1200, 500, 0};
    FactionCatalog catalog;
    catalog.describe({kurenai});
    CHECK(catalog.baseStanding(978, 11, 1) == 0);
    CHECK(catalog.baseStanding(978, 1, 1) == -1200);
    CHECK(catalog.baseStanding(978, 2, 4) == 500);
    CHECK(catalog.baseStanding(978, 2, 1) == 0);
}

TEST_CASE("The first change is measured from the standing the character logged in with", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    auto standings = metAll();
    standings[1].standing = 5000;
    rep.initialize(standings, catalog);
    auto changes = rep.changeStanding(1, 72, 5250, catalog);
    REQUIRE(changes.size() == 1);
    const auto& moved = std::get<StandingChanged>(changes[0]);
    CHECK(moved.factionName == "Stormwind");
    CHECK(moved.delta == 250);
    CHECK(moved.standing == 5250);
    CHECK(rep.watched() == 72);
    CHECK(rep.changeStanding(1, 72, 5250, catalog).empty());
}

TEST_CASE("A standing never changed since login is read by its index", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    auto standings = metAll();
    standings[3].standing = -1200;
    rep.initialize(standings, catalog);
    CHECK(rep.standingOf(978, catalog) == -1200);
}

TEST_CASE("The panel groups factions under the headings they descend from", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    rep.initialize(metAll(), catalog);
    const auto& rows = rep.rows(catalog);
    // Darnassus is index 2 and Stormwind 1, so Stormwind is first; Kurenai,
    // at 3, comes after the group whose first member is at 1.
    CHECK(names(rows) == std::vector<std::string>{"Classic", "Alliance", "Stormwind", "Darnassus", "Kurenai"});
    CHECK(rows[0].isHeader);
    CHECK_FALSE(rows[0].hasRep);
    CHECK(rows[1].isHeader);
    CHECK(rows[1].hasRep);
    CHECK(rows[2].isChild);
    CHECK_FALSE(rows[4].isChild);
}

TEST_CASE("A closed group keeps its heading and loses what is under it", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    rep.initialize(metAll(), catalog);
    (void)rep.rows(catalog);
    CHECK(rep.collapse(469, true));
    CHECK(names(rep.rows(catalog)) == std::vector<std::string>{"Classic", "Alliance", "Kurenai"});
    CHECK_FALSE(rep.collapse(469, true));
}

TEST_CASE("A faction met during the session joins the panel", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    auto standings = metAll();
    standings[3].flags = 0;
    rep.initialize(standings, catalog);
    CHECK(names(rep.rows(catalog)).back() == "Darnassus");
    rep.setVisible(3, true);
    CHECK(names(rep.rows(catalog)).back() == "Kurenai");
}

TEST_CASE("Declaring war shows on the row at once, and peace can follow", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    rep.initialize(metAll(), catalog);
    auto kurenaiRow = [&] { return rep.rows(catalog).back(); };
    CHECK((kurenaiRow().flags & kFactionAtWar) == 0);
    CHECK(rep.declareWar(3, true));
    CHECK((kurenaiRow().flags & kFactionAtWar) != 0);
    CHECK(rep.declareWar(3, false));
    CHECK((kurenaiRow().flags & kFactionAtWar) == 0);
}

TEST_CASE("War is not declared where the server forbids it", "[reputation]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ReputationService service{dbc, gateway, bus};
    auto standings = metAll();
    standings[3].flags |= kFactionPeaceForced;
    service.onInitialize(standings);
    service.setFactionAtWar(3, true);
    service.setFactionAtWar(1, true);
    CHECK(gateway.sent == std::vector<std::string>{"war 1 on"});
    CHECK((service.getStandingAt(3)->flags & kFactionAtWar) == 0);
}

TEST_CASE("Watching a faction tells the server by its index, and only in the world", "[reputation]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ReputationService service{dbc, gateway, bus};
    service.setWatchedFactionId(978);
    service.setWatchedFactionId(0);
    gateway.world = false;
    service.setWatchedFactionId(72);
    CHECK(gateway.sent == std::vector<std::string>{"watch 3", "watch -1"});
    CHECK(service.getWatchedFactionId() == 72);
}

TEST_CASE("Standings arrive earned and are kept whole", "[reputation]") {
    FakeDbc dbc;
    for (auto& r : dbc.rows) {
        if (r.id != 978) continue;
        r.hasBase = true;
        r.base = {1 << 0, 0, 0, 0, 0, 0, 0, 0, -1200, 0, 0, 0};
    }
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ReputationService service{dbc, gateway, bus};
    std::vector<StandingChanged> moved;
    std::vector<wowee::core::EventBus::Subscription> subs;
    subs.push_back(bus.subscribe<StandingChanged>([&](const StandingChanged& e) { moved.push_back(e); }));
    auto standings = metAll();
    standings[3].standing = 900;
    service.onInitialize(standings);
    CHECK(service.getFactionStanding(978) == -300);
    service.onStanding(3, 1000);
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].standing == -200);
    CHECK(moved[0].delta == 100);
}

TEST_CASE("Asking before Faction.dbc can be read does not give up on it", "[reputation]") {
    FakeDbc dbc;
    dbc.assets = false;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ReputationService service{dbc, gateway, bus};
    CHECK(service.getFactionName(978).empty());
    dbc.assets = true;
    CHECK(service.getFactionName(978) == "Kurenai");
}

TEST_CASE("A new login's standings replace the last character's", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe(world());
    Reputation rep;
    auto first = metAll();
    first[3].standing = 9000;
    rep.initialize(first, catalog);
    rep.initialize(std::vector<FactionStanding>(2), catalog);
    CHECK(rep.byFaction().count(978) == 0);
    CHECK(rep.standingOf(978, catalog) == 0);
}

TEST_CASE("The server's at-war flag is announced so hostility is judged again", "[reputation]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ReputationService service{dbc, gateway, bus};
    int judged = 0;
    std::vector<wowee::core::EventBus::Subscription> subs;
    subs.push_back(bus.subscribe<StandingsInitialized>([&](const auto&) { ++judged; }));
    subs.push_back(bus.subscribe<AtWarChanged>([&](const auto&) { ++judged; }));
    service.onInitialize(metAll());
    service.onAtWar(3, true);
    service.onAtWar(40, true);
    CHECK(judged == 2);
    CHECK((service.getStandingAt(3)->flags & kFactionAtWar) != 0);
}

TEST_CASE("A faction Faction.dbc does not name is called by its id", "[reputation]") {
    FactionCatalog catalog;
    catalog.describe({faction(1005, "", 4)});
    Reputation rep;
    rep.initialize(metAll(), catalog);
    auto changes = rep.changeStanding(4, 1005, 100, catalog);
    REQUIRE(changes.size() == 1);
    CHECK(std::get<StandingChanged>(changes[0]).factionName == "faction #1005");
}

TEST_CASE("The next character has none of the last one's standings, watched faction or closed groups",
          "[reputation]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    ReputationService service{dbc, gateway, bus};
    auto standings = metAll();
    standings[3].standing = 9000;
    service.onInitialize(standings);
    service.setWatchedFactionId(978);
    service.setFactionCollapsed(1118, true);
    REQUIRE_FALSE(service.getReputationRows().empty());

    service.beginCharacter();

    CHECK(service.getReputationRows().empty());
    CHECK(service.getFactionStanding(978) == 0);
    CHECK(service.getStandingAt(3) == nullptr);
    CHECK(service.getWatchedFactionId() == 0);
    CHECK(service.getCollapsedFactions().empty());
    // What Faction.dbc says is the game's, not the character's.
    CHECK(service.getFactionName(978) == "Kurenai");
}
