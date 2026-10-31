// The game data an expansion uses, end to end: what its list in
// expansion.json makes the game read, what stops the game, and what an
// expansion without a table offers instead.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_data.hpp"
#include "game/game_handler.hpp"
#include "game/player_handler.hpp"
#include "game/title_gateway.hpp"
#include "game/update_field_table.hpp"

#include <string>

using Catch::Matchers::ContainsSubstring;
using wowee::test::GameHarness;
using wowee::test::GameStopped;

TEST_CASE("a table the expansion uses that cannot be read stops the game, naming it and the expansion",
          "[game_data][harness]") {
    GameHarness h;
    h.unreadableDbc("Languages.dbc");
    h.enterWorld();

    CHECK_THROWS_MATCHES(h.game().getLanguageName(7), GameStopped,
                         Catch::Matchers::MessageMatches(ContainsSubstring("Languages.dbc could not be read") &&
                                                     ContainsSubstring("Wrath of the Lich King")));
}

TEST_CASE("a table narrower than the columns read stops the game", "[game_data][harness]") {
    GameHarness h;
    h.dbc("Languages.dbc", 1, {{{0, 7}}});
    h.enterWorld();

    CHECK_THROWS_MATCHES(h.game().getLanguageName(7), GameStopped,
                         Catch::Matchers::MessageMatches(ContainsSubstring("Languages.dbc has 1 fields, 2 are read")));
}

TEST_CASE("opening the expansion's data reads its whole list and stops at the first table that does not read",
          "[game_data][harness]") {
    GameHarness h;
    h.gameData().checkAll();  // every table there, if empty

    h.unreadableDbc("QuestSort.dbc");
    CHECK_THROWS_MATCHES(h.gameData().checkAll(), GameStopped,
                         Catch::Matchers::MessageMatches(ContainsSubstring("QuestSort.dbc could not be read")));
}

TEST_CASE("the expansion's own files are the ones its list names", "[game_data][harness]") {
    GameHarness h;
    CHECK_THAT(h.gameData().file("opcodes.json", "the test"), ContainsSubstring("expansions/wotlk/opcodes.json"));
    CHECK_THROWS_MATCHES(h.gameData().file("movement_sequences.json", "the test"), GameStopped,
                         Catch::Matchers::MessageMatches(ContainsSubstring("not on the expansion's list of files")));
}

TEST_CASE("a table the expansion does not use is never opened, and asking for it is a bug that stops the game",
          "[game_data][harness]") {
    GameHarness h;
    h.enterWorld();
    h.useExpansion("tbc");

    // Currencies are WotLK's: TBC has no tab to fill and no file to open.
    CHECK(h.game().getCurrencyTypes().empty());
    CHECK_FALSE(h.dbcWasRead("CurrencyTypes.dbc"));
    CHECK_FALSE(h.gameData().has("CurrencyTypes.dbc"));

    CHECK_THROWS_MATCHES(h.gameData().table("CurrencyTypes.dbc", 2, "the test"), GameStopped,
                         Catch::Matchers::MessageMatches(ContainsSubstring("not on the expansion's list of tables") &&
                                                     ContainsSubstring("Burning Crusade")));
}

TEST_CASE("Classic's character sheet has no numbers from game tables, and reads none",
          "[game_data][harness]") {
    using wowee::game::UF;
    GameHarness h;
    wowee::test::PlayerSpec mage;
    mage.classId = 8;
    h.enterWorld(mage);
    h.useExpansion("classic");

    CHECK(h.game().player().meleeCritFromAgility() == 0.0f);
    CHECK(h.game().player().manaRegenFromSpirit() == 0.0f);
    CHECK_FALSE(h.dbcWasRead("gtChanceToMeleeCrit.dbc"));
    CHECK_FALSE(h.dbcWasRead("gtRegenMPPerSpt.dbc"));
}

TEST_CASE("the currency tab lists the CurrencyTypes.dbc rows that name an item", "[game_data][harness]") {
    GameHarness h;
    h.dbc("CurrencyTypes.dbc", 4, {{{0, 1}, {1, 29434}}, {{0, 2}, {1, 0}}, {{0, 3}, {1, 40752}}});
    h.enterWorld();

    const auto& currencies = h.game().getCurrencyTypes();
    REQUIRE(currencies.size() == 2);
    CHECK(currencies[0].itemId == 29434);
    CHECK(currencies[1].itemId == 40752);
}

TEST_CASE("a vendor's honor, arena and token prices are ItemExtendedCost.dbc's", "[game_data][harness]") {
    GameHarness h;
    // ID, honor, arena, slot rule, five items at 4-8 and their counts at 9-13.
    h.dbc("ItemExtendedCost.dbc", 16, {{{0, 2425}, {1, 0}, {2, 1250}, {4, 40752}, {9, 25}}});
    h.enterWorld();

    const auto* cost = h.game().getExtendedCost(2425);
    REQUIRE(cost != nullptr);
    CHECK(cost->arenaPoints == 1250);
    CHECK(cost->itemId[0] == 40752);
    CHECK(cost->itemCount[0] == 25);
    CHECK(h.game().getExtendedCost(1) == nullptr);
}

TEST_CASE("the battleground list is BattlemasterList.dbc's battleground rows, not its arenas",
          "[game_data][harness]") {
    GameHarness h;
    // ID, maps at 1-8 (0xFFFFFFFF unused), type at 9, name at 11, group size
    // at 28, levels at 30 and 31.
    const uint32_t none = 0xFFFFFFFFu;
    h.dbc("BattlemasterList.dbc", 32, {
        {{0, 2}, {1, 489}, {2, none}, {3, none}, {4, none}, {5, none}, {6, none}, {7, none}, {8, none},
         {9, 3}, {11, "Warsong Gulch"}, {28, 10}, {30, 10}, {31, 80}},
        {{0, 4}, {1, 559}, {2, none}, {3, none}, {4, none}, {5, none}, {6, none}, {7, none}, {8, none},
         {9, 4}, {11, "Nagrand Arena"}, {28, 5}, {30, 10}, {31, 80}},
    });
    h.enterWorld();

    const auto& battlegrounds = h.game().getBattlegroundTypes();
    REQUIRE(battlegrounds.size() == 1);
    CHECK(battlegrounds[0].name == "Warsong Gulch");
    CHECK(h.game().isBattlegroundMap(489));
    CHECK(h.game().isArenaMap(559));
    CHECK_FALSE(h.game().isBattlegroundMap(559));
}

TEST_CASE("an instance's boss count is its DungeonEncounter.dbc rows", "[game_data][harness]") {
    GameHarness h;
    // ID, map, difficulty.
    h.dbc("DungeonEncounter.dbc", 23, {{{0, 464}, {1, 33}, {2, 0}}, {{0, 465}, {1, 33}, {2, 0}},
                                       {{0, 466}, {1, 33}, {2, 1}}});
    h.enterWorld();

    CHECK(h.game().getDungeonEncounterCount(33, 0) == 2);
    CHECK(h.game().getDungeonEncounterCount(33, 1) == 1);
    CHECK(h.game().getDungeonEncounterCount(36, 0) == 0);
}

TEST_CASE("a dungeon finder entry is named by LFGDungeons.dbc", "[game_data][harness]") {
    GameHarness h;
    h.dbc("LFGDungeons.dbc", 49, {{{0, 18}, {1, "Scarlet Monastery"}, {23, 189}, {31, 1}}});
    h.enterWorld();

    CHECK(h.game().getLfgDungeonName(18) == "Scarlet Monastery");
    CHECK(h.game().getLfgDungeonName(19).empty());
}

TEST_CASE("a glyph is the spell GlyphProperties.dbc gives it", "[game_data][harness]") {
    GameHarness h;
    h.dbc("GlyphProperties.dbc", 4, {{{0, 161}, {1, 54825}}});
    h.enterWorld();

    h.game().ensureGlyphPropertiesLoaded();
    CHECK(h.game().getGlyphSpellId(161) == 54825);
}

TEST_CASE("the titles are CharTitles.dbc's names by their bit", "[game_data][harness]") {
    GameHarness h;
    // ID, condition, the name at 2, its bit at 36.
    h.dbc("CharTitles.dbc", 37, {{{0, 1}, {2, "Private %s"}, {36, 1}}});
    h.enterWorld();

    wowee::game::TitleData data(h.gameData());
    const auto titles = data.titles();
    REQUIRE(titles.size() == 1);
    CHECK(titles[0].format == "Private %s");
    CHECK(titles[0].bit == 1);
}
