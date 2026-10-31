// Reputation, end to end: the standings the server sends at login and as they
// move, what the chat, the reputation panel and the hostility of the units
// around the player read from them, and the requests that change them.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/entity.hpp"
#include "game/faction_hostility.hpp"
#include "game/game_handler.hpp"
#include "game/reputation_gateway.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packet_data.hpp"
#include "reputation/domain/records.hpp"
#include "reputation/domain/standings.hpp"
#include "reputation/services/reputation.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::fieldIndex;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint8_t kVisible = wowee::reputation::kFactionVisible;
constexpr uint8_t kAtWar = wowee::reputation::kFactionAtWar;
constexpr uint32_t kNoIndex = wowee::reputation::kNoReputationIndex;

wowee::reputation::FactionRecord faction(uint32_t id, const std::string& name, uint32_t index,
                                         uint32_t parent = 0) {
    wowee::reputation::FactionRecord r;
    r.id = id;
    r.name = name;
    r.reputationIndex = index;
    r.parentId = parent;
    return r;
}

/// Faction.dbc: Alliance (index 0) holds Stormwind (index 1); the Kurenai
/// (index 2) stand alone and start a human at -1200.
void factionFiles(GameHarness& h) {
    auto kurenai = faction(978, "Kurenai", 2);
    kurenai.hasBase = true;
    kurenai.base = {1 << 0, 0, 0, 0, 0, 0, 0, 0, -1200, 0, 0, 0};
    h.factionFiles().rows = {faction(469, "Alliance", 0), faction(72, "Stormwind", 1, 469), kurenai,
                             faction(1118, "Classic", kNoIndex)};
}

/// SMSG_INITIALIZE_FACTIONS: how many, then a flags byte and the earned
/// standing for each, by ReputationListID.
std::vector<uint8_t> initializeFactions(const std::vector<std::pair<uint8_t, int32_t>>& factions) {
    Body b;
    b.u32(static_cast<uint32_t>(factions.size()));
    for (const auto& [flags, standing] : factions) b.u8(flags).u32(static_cast<uint32_t>(standing));
    return b.bytes();
}

/// SMSG_SET_FACTION_STANDING as 3.3.5a writes it: the refer-a-friend bonus,
/// whether to show it, then one earned standing by ReputationListID.
std::vector<uint8_t> standingMoved(uint32_t index, int32_t earned) {
    return Body().f32(0.0f).u8(0).u32(1).u32(index).u32(static_cast<uint32_t>(earned)).bytes();
}

size_t linesSaying(GameHarness& h, const std::string& text, wowee::game::ChatType type) {
    const auto& history = h.game().chat().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(), [&](const auto& line) {
        return line.message == text && line.type == type;
    }));
}

std::vector<std::string> rowNames(GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& row : h.reputation().getReputationRows()) out.push_back(row.name);
    return out;
}

}  // namespace

TEST_CASE("the standings at login are whole and are what the reputation reads", "[reputation][harness]") {
    GameHarness h;
    factionFiles(h);
    h.enterWorld();   // a human warrior
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS,
              initializeFactions({{kVisible, 100}, {kVisible, 5000}, {kVisible | kAtWar, 900}}));
    h.pump();

    REQUIRE(h.reputation().getStandingAt(2));
    CHECK((h.reputation().getStandingAt(2)->flags & kAtWar) != 0);
    CHECK(h.reputation().getFactionStanding(72) == 5000);
    // Earned on top of where a human starts with the Kurenai.
    CHECK(h.reputation().getFactionStanding(978) == -300);

    // SMSG_SET_FACTION_ATWAR: the faction, then war or peace.
    h.receive(Opcode::SMSG_SET_FACTION_ATWAR, Body().u32(2).u8(0));
    h.pump();
    CHECK((h.reputation().getStandingAt(2)->flags & kAtWar) == 0);
}

TEST_CASE("a standing that moves is said, told to the panel and watched", "[reputation][harness]") {
    GameHarness h;
    factionFiles(h);
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible, 0}, {kVisible, 5000}}));
    h.pump();

    h.receive(Opcode::SMSG_SET_FACTION_STANDING, standingMoved(1, 5250));
    h.pump();

    const std::string line = "Reputation with Stormwind increased by 250.";
    CHECK(linesSaying(h, line, wowee::game::ChatType::SYSTEM) == 1);
    CHECK(linesSaying(h, line, wowee::game::ChatType::COMBAT_FACTION_CHANGE) == 1);
    CHECK(h.addonEvents("UPDATE_FACTION").size() == 1);
    CHECK(h.reputation().getFactionStanding(72) == 5250);
    CHECK(h.reputation().getWatchedFactionId() == 72);

    // The same standing again moved nothing and says nothing.
    h.receive(Opcode::SMSG_SET_FACTION_STANDING, standingMoved(1, 5250));
    h.pump();
    CHECK(h.addonEvents("UPDATE_FACTION").size() == 1);
}

TEST_CASE("a standing at an index Faction.dbc does not know is not shown", "[reputation][harness]") {
    GameHarness h;
    factionFiles(h);
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible, 0}}));
    h.pump();

    h.receive(Opcode::SMSG_SET_FACTION_STANDING, standingMoved(40, 300));
    h.pump();

    CHECK(h.addonEvents("UPDATE_FACTION").empty());
    CHECK(h.reputation().getWatchedFactionId() == 0);
}

TEST_CASE("the panel groups the factions met, and one met later joins it at once", "[reputation][harness]") {
    GameHarness h;
    factionFiles(h);
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible, 0}, {kVisible, 0}, {0, 0}}));
    h.pump();
    CHECK(rowNames(h) == std::vector<std::string>{"Alliance", "Stormwind"});

    // SMSG_SET_FACTION_VISIBLE: the faction, then whether it is shown.
    h.receive(Opcode::SMSG_SET_FACTION_VISIBLE, Body().u32(2).u8(1));
    h.pump();
    CHECK(rowNames(h) == std::vector<std::string>{"Alliance", "Stormwind", "Kurenai"});

    h.reputation().setFactionCollapsed(469, true);
    CHECK(rowNames(h) == std::vector<std::string>{"Alliance", "Kurenai"});
}

TEST_CASE("declaring war asks the server and shows on the row at once, and peace follows",
          "[reputation][harness]") {
    GameHarness h;
    factionFiles(h);
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible, 0}, {kVisible, 0}, {kVisible, 0}}));
    h.pump();
    const auto kurenaiRow = [&]() {
        for (const auto& row : h.reputation().getReputationRows())
            if (row.factionId == 978) return row;
        FAIL("no Kurenai row");
        return wowee::reputation::ReputationRow{};
    };

    h.reputation().setFactionAtWar(2, true);
    auto* war = h.lastSent(Opcode::CMSG_SET_FACTION_ATWAR);
    REQUIRE(war);
    CHECK(war->readUInt32() == 2);
    CHECK(war->readUInt8() == 1);
    CHECK((kurenaiRow().flags & kAtWar) != 0);

    h.clearRecords();
    h.reputation().setFactionAtWar(2, false);
    auto* peace = h.lastSent(Opcode::CMSG_SET_FACTION_ATWAR);
    REQUIRE(peace);
    CHECK(peace->readUInt32() == 2);
    CHECK(peace->readUInt8() == 0);
    CHECK((kurenaiRow().flags & kAtWar) == 0);
}

TEST_CASE("a unit of a faction the player is at war with is hostile, and peace makes it not",
          "[reputation][harness]") {
    constexpr uint64_t kVindicator = 0xF130000000004321ull;
    constexpr uint32_t kKurenaiTemplate = 1726;
    GameHarness h;
    factionFiles(h);
    h.enterWorld();
    h.game().setFactionTemplateRepList({{kKurenaiTemplate, 2}});
    // What FactionTemplate.dbc says from the start: not hostile.
    h.game().setFactionHostileMap({{kKurenaiTemplate, false}});
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kVindicator, wowee::game::ObjectType::UNIT,
                                            {{fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE), kKurenaiTemplate},
                                             {fieldIndex(UF::UNIT_FIELD_HEALTH), 100}}));
    h.pump();
    auto unit = [&] {
        return std::static_pointer_cast<wowee::game::Unit>(h.game().getEntityManager().getEntity(kVindicator));
    };
    REQUIRE(unit());
    REQUIRE_FALSE(unit()->isHostile());

    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible, 0}, {kVisible, 0}, {kVisible | kAtWar, 0}}));
    h.pump();
    CHECK(unit()->isHostile());

    h.receive(Opcode::SMSG_SET_FACTION_ATWAR, Body().u32(2).u8(0));
    h.pump();
    CHECK_FALSE(unit()->isHostile());
}

TEST_CASE("the next character has none of the last one's reputation", "[reputation][harness]") {
    PlayerSpec alice;
    alice.guid = 0x81;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x82;
    bob.name = "Bob";

    GameHarness h;
    factionFiles(h);
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_INITIALIZE_FACTIONS, initializeFactions({{kVisible, 0}, {kVisible, 0}, {kVisible | kAtWar, 0}}));
    h.pump();
    h.reputation().setWatchedFactionId(72);
    h.reputation().setFactionCollapsed(469, true);
    REQUIRE(h.reputation().getStandingAt(2));

    h.relogAs(bob);

    // Until Bob's own standings arrive, Alice's at-war flags judged who was
    // hostile to him, and her watched faction was on his reputation bar.
    CHECK(h.reputation().getStandingAt(2) == nullptr);
    CHECK(h.reputation().getReputationRows().empty());
    CHECK(h.reputation().getWatchedFactionId() == 0);
    CHECK(h.reputation().getCollapsedFactions().empty());
}

TEST_CASE("who is hostile and who friendly to a human is what FactionTemplate.dbc's groups say",
          "[reputation][harness][tables]") {
    GameHarness h;
    // ID, faction, flags, faction group, friend group, enemy group. The groups
    // are bits: 1 player, 2 Alliance, 4 Horde, 8 monster.
    h.dbc("FactionTemplate.dbc", 14, {
        {{0, 1}, {1, 1}, {3, 3}, {4, 2}, {5, 4}},     // a human player
        {{0, 11}, {1, 72}, {3, 2}, {4, 2}, {5, 4}},   // a Stormwind guard
        {{0, 85}, {1, 29}, {3, 4}, {4, 4}, {5, 2}},   // an Orgrimmar grunt
        {{0, 14}, {1, 20}, {3, 8}},                   // a monster
        {{0, 31}, {1, 31}},                           // wildlife
    });
    h.enterWorld();

    wowee::game::buildFactionHostilityMap(h.gameData(), h.game(), 1 /* human */);

    CHECK(h.game().isHostileFaction(85));
    CHECK(h.game().isHostileFaction(14));
    CHECK_FALSE(h.game().isHostileFaction(11));
    CHECK(h.game().isFriendlyFaction(11));
    CHECK_FALSE(h.game().isHostileFaction(31));
    CHECK_FALSE(h.game().isFriendlyFaction(31));
}

TEST_CASE("the reputation panel's factions are Faction.dbc's rows, named and grouped",
          "[reputation][harness][tables]") {
    GameHarness h;
    // ID, standing index, the race masks and bases, parent at 18, name at 23.
    h.dbc("Faction.dbc", 57, {
        {{0, 469}, {1, 0xFFFFFFFFu}, {23, "Alliance"}},
        {{0, 72}, {1, 7}, {18, 469}, {23, "Stormwind"}},
    });
    h.enterWorld();

    wowee::game::FactionData data(h.gameData());
    const auto factions = data.factions();
    const auto stormwind = std::find_if(factions.begin(), factions.end(),
                                        [](const auto& f) { return f.id == 72; });
    REQUIRE(stormwind != factions.end());
    CHECK(stormwind->name == "Stormwind");
    CHECK(stormwind->parentId == 469);
    CHECK(stormwind->reputationIndex == 7);
}
