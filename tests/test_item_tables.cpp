// What an item tooltip reads from the expansion's tables (ui/item_tables.hpp):
// names through the expansion's own column layout, a table the expansion does
// not use left unopened, and a column the layout does not name stopping the
// game rather than being read from a column number written into the client.
#include <catch_amalgamated.hpp>

#include "pipeline/required_table.hpp"
#include "test_game_tables.hpp"
#include "ui/item_tables.hpp"

using wowee::test::GivenLayouts;
using wowee::test::GivenTables;
using wowee::test::TablesStopped;

namespace {

/// Classic's Faction.dbc keeps the name at 19, WotLK's at 23. The tooltip read
/// column 20 for every expansion, which is neither.
constexpr const char* kClassicFaction = R"({"Faction": {"ID": 0, "Name": 19}})";
constexpr const char* kWotlkFaction = R"({"Faction": {"ID": 0, "Name": 23}})";

}  // namespace

TEST_CASE("a faction's name comes from the column the expansion's layout names", "[tables][tooltip]") {
    SECTION("Classic") {
        const GivenLayouts layouts(kClassicFaction);
        GivenTables tables;
        tables.give("Faction.dbc", 24, {{{0, 529}, {19, "Argent Dawn"}, {20, "wrong"}}});
        CHECK(wowee::ui::factionNames(tables).at(529) == "Argent Dawn");
    }
    SECTION("WotLK") {
        const GivenLayouts layouts(kWotlkFaction);
        GivenTables tables;
        tables.give("Faction.dbc", 24, {{{0, 1106}, {20, "wrong"}, {23, "Argent Crusade"}}});
        CHECK(wowee::ui::factionNames(tables).at(1106) == "Argent Crusade");
    }
}

TEST_CASE("a layout that does not name the faction's name column stops the game", "[tables][tooltip]") {
    const GivenLayouts layouts(R"({"Faction": {"ID": 0}})");
    GivenTables tables;
    tables.give("Faction.dbc", 24, {{{0, 529}, {20, "Argent Dawn"}}});
    CHECK_THROWS_AS(wowee::ui::factionNames(tables), TablesStopped);
}

TEST_CASE("a Faction.dbc narrower than its layout stops the game", "[tables][tooltip]") {
    const GivenLayouts layouts(kWotlkFaction);
    GivenTables tables;
    tables.give("Faction.dbc", 20, {{{0, 529}}});
    CHECK_THROWS_AS(wowee::ui::factionNames(tables), TablesStopped);
}

TEST_CASE("names are read again from a table that replaced the one they came from", "[tables][tooltip]") {
    // An expansion change replaces every table; the names must follow it.
    const GivenLayouts layouts(R"({"SkillLine": {"ID": 0, "Name": 3}})");
    GivenTables before;
    before.give("SkillLine.dbc", 4, {{{0, 164}, {3, "Blacksmithing"}}});
    CHECK(wowee::ui::skillLineNames(before).at(164) == "Blacksmithing");

    GivenTables after;
    after.give("SkillLine.dbc", 4, {{{0, 164}, {3, "Forge"}}});
    CHECK(wowee::ui::skillLineNames(after).at(164) == "Forge");
}

TEST_CASE("an expansion without ItemSet.dbc shows no set and never opens the file", "[tables][tooltip]") {
    const GivenLayouts layouts(R"({"Faction": {"ID": 0, "Name": 19}})");
    GivenTables tables;
    CHECK(wowee::ui::itemSets(tables).empty());
    CHECK_FALSE(tables.opened("ItemSet.dbc"));
}

TEST_CASE("an item set's pieces and bonuses are read where the expansion's layout puts them",
          "[tables][tooltip]") {
    // A layout that keeps the pieces at 10 onward, where WotLK has names in other
    // languages; the tooltip read WotLK's 18 whenever there was no layout.
    std::string layout = R"({"ItemSet": {"ID": 0, "Name": 1)";
    for (int i = 0; i < 10; ++i) {
        layout += ", \"Item" + std::to_string(i) + "\": " + std::to_string(10 + i);
        layout += ", \"Spell" + std::to_string(i) + "\": " + std::to_string(27 + i);
        layout += ", \"Threshold" + std::to_string(i) + "\": " + std::to_string(37 + i);
    }
    layout += "}}";
    const GivenLayouts layouts(layout);
    GivenTables tables;
    tables.give("ItemSet.dbc", 47,
                {{{0, 81}, {1, "Battlegear of Might"}, {10, 16866}, {11, 16867}, {27, 21838}, {37, 3}}});

    const auto& sets = wowee::ui::itemSets(tables);
    REQUIRE(sets.count(81) == 1);
    const auto& set = sets.at(81);
    CHECK(set.name == "Battlegear of Might");
    CHECK(set.itemIds[0] == 16866);
    CHECK(set.itemIds[1] == 16867);
    CHECK(set.spellIds[0] == 21838);
    CHECK(set.thresholds[0] == 3);
}
