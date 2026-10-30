// The player, end to end: what the server's fields say of the character -
// experience, level, rest, the sheet's numbers, the helm and cloak, the form -
// and what the interface is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/chat_handler.hpp"
#include "game/entity_controller.hpp"
#include "game/game_events.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::game::UF;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {
uint16_t uf(UF f) { return static_cast<uint16_t>(wowee::game::fieldIndex(f)); }
uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

std::vector<std::string> names(const GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.addonEvents()) out.push_back(e.name);
    return out;
}
bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

constexpr uint32_t kResting = 0x20, kHideHelm = 0x400, kHideCloak = 0x800;
}  // namespace

TEST_CASE("experience gained is shown with the new total; the login's is not news", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::PLAYER_XP), 300}, {uf(UF::PLAYER_NEXT_LEVEL_XP), 7600}}, /*create=*/true);
    CHECK(h.game().player().xp() == 300);
    CHECK(h.game().player().nextLevelXp() == 7600);
    CHECK(h.addonEvents("PLAYER_XP_UPDATE").empty());

    h.playerFields({{uf(UF::PLAYER_XP), 420}});
    CHECK(h.game().player().xp() == 420);
    const auto xp = h.addonEvents("PLAYER_XP_UPDATE");
    REQUIRE(xp.size() == 1);
    CHECK(xp[0].args == std::vector<std::string>{"420"});
}

TEST_CASE("the level the fields give is the player's, and the character list's", "[player][harness]") {
    GameHarness h;
    h.enterWorld(PlayerSpec{.level = 10});
    h.playerInView();
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 11}});
    CHECK(h.game().player().level() == 11);
    const auto& list = h.game().getCharacters();
    const auto me = std::find_if(list.begin(), list.end(),
                                 [&](const auto& c) { return c.guid == h.player().guid; });
    REQUIRE(me != list.end());
    CHECK(me->level == 11);
}

TEST_CASE("logging in at an inn is resting already; leaving it says so", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::PLAYER_FLAGS), kResting}}, /*create=*/true);
    CHECK(h.game().player().isResting());
    CHECK(h.addonEvents("PLAYER_UPDATE_RESTING").size() == 1);
    // A create block says what is, not what moved.
    CHECK(h.addonEvents("PLAYER_FLAGS_CHANGED").empty());
    h.clearRecords();

    h.playerFields({{uf(UF::PLAYER_FLAGS), 0}});
    CHECK_FALSE(h.game().player().isResting());
    CHECK(h.addonEvents("PLAYER_UPDATE_RESTING").size() == 1);
    const auto changed = h.addonEvents("PLAYER_FLAGS_CHANGED");
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].args == std::vector<std::string>{"player"});
}

TEST_CASE("the helm and cloak are shown as the realm keeps them", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    CHECK(h.game().player().helmVisible());
    CHECK(h.game().player().cloakVisible());

    h.playerFields({{uf(UF::PLAYER_FLAGS), kHideHelm}}, /*create=*/true);
    CHECK_FALSE(h.game().player().helmVisible());
    CHECK(h.game().player().cloakVisible());

    h.playerFields({{uf(UF::PLAYER_FLAGS), kHideCloak}});
    CHECK(h.game().player().helmVisible());
    CHECK_FALSE(h.game().player().cloakVisible());
}

TEST_CASE("the character sheet is told what moved, once, and not of what stayed", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::UNIT_FIELD_STAT0), 40},
                     {uf(UF::UNIT_FIELD_RESISTANCES), 500},
                     {uf(UF::UNIT_FIELD_RANGED_ATTACK_POWER), 90}},
                 /*create=*/true);
    CHECK(h.game().player().stat(0) == 40);
    CHECK(h.game().player().armor() == 500);
    CHECK(h.game().player().rangedAttackPower() == 90);
    // The sheet is built from what is there; nothing to redraw at login.
    CHECK(h.addonEvents().empty());

    // Strength and fire resistance move; armor is sent again unchanged.
    h.playerFields({{uf(UF::UNIT_FIELD_STAT0), 45},
                     {uf(UF::UNIT_FIELD_RESISTANCES), 500},
                     {static_cast<uint16_t>(uf(UF::UNIT_FIELD_RESISTANCES) + 2), 15}});
    CHECK(h.game().player().stat(0) == 45);
    CHECK(h.game().player().resistance(2) == 15);
    CHECK(h.addonEvents("UNIT_STATS").size() == 1);
    CHECK(h.addonEvents("UNIT_RESISTANCES").size() == 1);
    CHECK(h.addonEvents("UNIT_RANGED_ATTACK_POWER").empty());
    h.clearRecords();

    // Ranged attack power has its own event; the same values again are no news.
    h.playerFields({{uf(UF::UNIT_FIELD_STAT0), 45}, {uf(UF::UNIT_FIELD_RANGED_ATTACK_POWER), 120}});
    CHECK(h.game().player().rangedAttackPower() == 120);
    const auto ranged = h.addonEvents("UNIT_RANGED_ATTACK_POWER");
    REQUIRE(ranged.size() == 1);
    CHECK(ranged[0].args == std::vector<std::string>{"player"});
    CHECK(h.addonEvents("UNIT_STATS").empty());
    CHECK(h.addonEvents("UNIT_ATTACK_POWER").empty());
}

TEST_CASE("ratings and spell power redraw their panels", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{static_cast<uint16_t>(uf(UF::PLAYER_FIELD_COMBAT_RATING_1) + 3), 25},
                     {static_cast<uint16_t>(uf(UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS) + 2), 120}});
    CHECK(h.game().player().combatRating(3) == 25);
    CHECK(h.game().player().spellPower() == 120);
    CHECK(h.addonEvents("COMBAT_RATING_UPDATE").size() == 1);
    CHECK(h.addonEvents("PLAYER_DAMAGE_DONE_MODS").size() == 1);
}

TEST_CASE("a chance past a hundred percent is shown as a hundred", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::PLAYER_DODGE_PERCENTAGE), bits(12.5f)},
                     {uf(UF::PLAYER_PARRY_PERCENTAGE), bits(250.0f)}});
    CHECK(h.game().player().dodgePct() == 12.5f);
    CHECK(h.game().player().parryPct() == 100.0f);
}

TEST_CASE("one update's events come in the order of its fields", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::PLAYER_FLAGS), 0}, {uf(UF::PLAYER_XP), 0}, {uf(UF::PLAYER_FIELD_COINAGE), 0}},
                 /*create=*/true);
    h.clearRecords();

    // Into an inn with the reward of a quest: the flags, then the experience,
    // then the coin.
    h.playerFields({{uf(UF::PLAYER_FLAGS), kResting},
                     {uf(UF::PLAYER_XP), 900},
                     {uf(UF::PLAYER_FIELD_COINAGE), 2500}});
    std::vector<std::string> order;
    for (const auto& n : names(h)) {
        if (n == "PLAYER_UPDATE_RESTING" || n == "PLAYER_FLAGS_CHANGED" ||
            n == "PLAYER_XP_UPDATE" || n == "PLAYER_MONEY")
            order.push_back(n);
    }
    CHECK(order == std::vector<std::string>{"PLAYER_UPDATE_RESTING", "PLAYER_FLAGS_CHANGED",
                                            "PLAYER_XP_UPDATE", "PLAYER_MONEY"});
}

TEST_CASE("a change of form moves the stance bar and pages the action bar", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.clearRecords();
    // UNIT_FIELD_BYTES_1: the form is the top byte. Bear form is 5.
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 5u << 24}});
    CHECK(h.game().player().shapeshiftForm() == 5);
    const auto all = names(h);
    for (const char* e : {"UPDATE_SHAPESHIFT_FORM", "UPDATE_SHAPESHIFT_FORMS",
                          "UPDATE_BONUS_ACTIONBAR", "ACTIONBAR_PAGE_CHANGED"})
        CHECK(std::count(all.begin(), all.end(), e) == 1);
    h.clearRecords();

    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 5u << 24}});
    CHECK_FALSE(has(names(h), "UPDATE_SHAPESHIFT_FORM"));
}

TEST_CASE("the next character starts with none of the last one's sheet", "[player][harness]") {
    GameHarness h;
    h.useExpansion("wotlk");
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::PLAYER_FIELD_HONOR_CURRENCY), 1500},
                     {uf(UF::PLAYER_FIELD_ARENA_CURRENCY), 300},
                     {uf(UF::PLAYER_REST_STATE_EXPERIENCE), 8000},
                     {uf(UF::PLAYER_FLAGS), kResting | kHideHelm},
                     {uf(UF::UNIT_FIELD_STAT0), 40},
                     {uf(UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER), bits(4.5f)}},
                 /*create=*/true);
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 5u << 24}});
    REQUIRE(h.game().player().shapeshiftForm() == 5);
    REQUIRE(h.game().player().honorPoints() == 1500);

    // A create block leaves out every field that is zero, so the next
    // character's says nothing of honor, rest, the helm or the form.
    PlayerSpec bob;
    bob.guid = 0x11;
    bob.name = "Bob";
    h.relogAs(bob);
    h.playerInView();
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 10}}, /*create=*/true);

    CHECK(h.game().player().honorPoints() == 0);
    CHECK(h.game().player().arenaPoints() == 0);
    CHECK(h.game().player().restedXp() == 0);
    CHECK_FALSE(h.game().player().isResting());
    CHECK(h.game().player().helmVisible());
    CHECK(h.game().player().stat(0) == -1);
    CHECK(h.game().player().shapeshiftForm() == 0);
    CHECK(h.game().player().manaRegen() == 0.0f);
}

TEST_CASE("a level gained is announced with what it brought; an old one again is not", "[player][harness]") {
    GameHarness h;
    h.enterWorld(PlayerSpec{.level = 10});
    h.playerInView();
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 10}}, /*create=*/true);
    std::vector<wowee::game::events::PlayerLevelUp> dings;
    auto sub = h.events().subscribe<wowee::game::events::PlayerLevelUp>(
        [&](const auto& e) { dings.push_back(e); });

    // SMSG_LEVELUP_INFO: the level, health and power gained, then the five stats.
    h.receive(Opcode::SMSG_LEVELUP_INFO,
              Body().u32(11).u32(18).u32(25).u32(0).u32(0).u32(1).u32(2).u32(1).bytes());
    h.pump();
    CHECK(h.game().player().level() == 11);
    REQUIRE(dings.size() == 1);
    CHECK(dings[0].oldLevel == 10);
    CHECK(dings[0].newLevel == 11);
    const auto shown = h.addonEvents("PLAYER_LEVEL_UP");
    REQUIRE(shown.size() == 1);
    // Level, health, power, talent points, then strength to spirit.
    CHECK(shown[0].args == std::vector<std::string>{"11", "18", "25", "0", "0", "0", "1", "2", "1"});
    h.clearRecords();

    h.receive(Opcode::SMSG_LEVELUP_INFO, Body().u32(11).bytes());
    h.pump();
    CHECK(dings.size() == 1);
    CHECK(h.addonEvents("PLAYER_LEVEL_UP").empty());
}

TEST_CASE("entering an inn says the player is resting, once", "[player][harness]") {
    GameHarness h;
    h.useExpansion("wotlk");
    h.enterWorld();
    // WotLK's SMSG_SET_REST_START, on the opcode older realms use to drop a quest.
    h.receive(Opcode::SMSG_QUEST_FORCE_REMOVE, Body().u32(1).bytes());
    h.receive(Opcode::SMSG_QUEST_FORCE_REMOVE, Body().u32(1).bytes());
    h.pump();
    CHECK(h.game().player().isResting());
    CHECK(h.addonEvents("PLAYER_UPDATE_RESTING").size() == 1);
    const auto& history = h.game().chat().getChatHistory();
    CHECK(std::count_if(history.begin(), history.end(), [](const auto& line) {
              return line.message == "You are now resting.";
          }) == 1);
}
