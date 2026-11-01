// The player, end to end: what the server's fields say of the character -
// experience, level, rest, the sheet's numbers, the helm and cloak, the form -
// and what the interface is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/chat_handler.hpp"
#include "game/world_handler.hpp"
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

namespace {
// A per-class table where only the warrior's row is the one asked for: any
// other row read gives a number far from what the test expects.
std::vector<float> perClass(float warrior) {
    std::vector<float> rows(11, 9.0f);
    rows[0] = warrior;
    return rows;
}
// A per-class-per-level table (11 classes x 100 levels), the warrior's row at
// `level` set.
std::vector<float> perClassAndLevel(uint32_t level, float value) {
    std::vector<float> rows(11 * 100, 9.0f);
    rows[level - 1] = value;
    return rows;
}
}  // namespace

TEST_CASE("crit from Agility and Intellect is the class base plus the stat at the level's ratio",
          "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.gameTable("gtChanceToMeleeCritBase.dbc", 0, perClass(0.05f));
    h.gameTable("gtChanceToMeleeCrit.dbc", 0, perClassAndLevel(10, 0.0005f));
    h.gameTable("gtChanceToSpellCritBase.dbc", 0, perClass(0.01f));
    h.gameTable("gtChanceToSpellCrit.dbc", 0, perClassAndLevel(10, 0.0002f));
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 10}, {uf(UF::UNIT_FIELD_STAT1), 40}, {uf(UF::UNIT_FIELD_STAT3), 100}},
                   /*create=*/true);
    // (0.05 + 40 x 0.0005) x 100, and (0.01 + 100 x 0.0002) x 100.
    CHECK(h.game().player().meleeCritFromAgility() == Catch::Approx(7.0f));
    CHECK(h.game().player().spellCritFromIntellect() == Catch::Approx(3.0f));
}

TEST_CASE("a level past the tables reads their last row", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.gameTable("gtChanceToMeleeCritBase.dbc", 0, perClass(0.05f));
    h.gameTable("gtChanceToMeleeCrit.dbc", 0, perClassAndLevel(100, 0.001f));
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 120}, {uf(UF::UNIT_FIELD_STAT1), 40}}, /*create=*/true);
    CHECK(h.game().player().meleeCritFromAgility() == Catch::Approx(9.0f));
}

TEST_CASE("regen from Spirit splits health at fifty Spirit and doubles it", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.gameTable("gtOCTRegenHP.dbc", 0, perClassAndLevel(10, 0.5f));
    h.gameTable("gtRegenHPPerSpt.dbc", 0, perClassAndLevel(10, 0.25f));
    h.gameTable("gtRegenMPPerSpt.dbc", 0, perClassAndLevel(10, 0.1f));
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 10}, {uf(UF::UNIT_FIELD_STAT4), 70}}, /*create=*/true);
    // (50 x 0.5 + 20 x 0.25) x 2, and 70 x 0.1.
    CHECK(h.game().player().healthRegenFromSpirit() == Catch::Approx(60.0f));
    CHECK(h.game().player().manaRegenFromSpirit() == Catch::Approx(7.0f));
}

TEST_CASE("a combat rating is worth the class scalar over the level's coefficient", "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    std::vector<float> coefficients(32 * 100, 9.0f);
    coefficients[2 * 100 + 9] = 2.5f;  // dodge at level 10
    coefficients[3 * 100 + 9] = 0.0f;  // parry at level 10: worth nothing
    h.gameTable("gtCombatRatings.dbc", 0, coefficients);
    std::vector<float> scalars(11 * 32, 9.0f);
    scalars[2] = 1.0f;  // the warrior's dodge
    // The scalar table is id then ratio: the ratio is what is read.
    h.gameTable("gtOCTClassCombatRatingScalar.dbc", 1, scalars);
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 10},
                    {static_cast<uint16_t>(uf(UF::PLAYER_FIELD_COMBAT_RATING_1) + 2), 25},
                    {static_cast<uint16_t>(uf(UF::PLAYER_FIELD_COMBAT_RATING_1) + 3), 25}},
                   /*create=*/true);
    // 25 x (1.0 / 2.5).
    CHECK(h.game().player().combatRatingBonus(2) == Catch::Approx(10.0f));
    CHECK(h.game().player().combatRatingBonus(3) == 0.0f);
    // A rating that does not exist is worth nothing, and reads no table.
    CHECK(h.game().player().combatRatingBonus(-1) == 0.0f);
    CHECK(h.game().player().combatRatingBonus(32) == 0.0f);
}

TEST_CASE("before the character is known nothing is read from the tables", "[player][harness]") {
    GameHarness h;
    // No character in the world, and no tables given: reading one would fail.
    CHECK(h.game().player().meleeCritFromAgility() == 0.0f);
    CHECK(h.game().player().healthRegenFromSpirit() == 0.0f);
    CHECK(h.game().player().combatRatingBonus(2) == 0.0f);
}

TEST_CASE("the player's fields are what the create block held, with each update over it",
          "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.playerFields({{uf(UF::PLAYER_XP), 300}, {uf(UF::UNIT_FIELD_LEVEL), 10}}, /*create=*/true);
    h.playerFields({{uf(UF::PLAYER_XP), 420}});
    const auto& fields = h.game().player().fields();
    CHECK(fields.find(uf(UF::PLAYER_XP))->second == 420);
    CHECK(fields.find(uf(UF::UNIT_FIELD_LEVEL))->second == 10);

    // The next character's are its own.
    PlayerSpec bob;
    bob.guid = 0x11;
    bob.name = "Bob";
    h.relogAs(bob);
    h.playerInView();
    h.playerFields({{uf(UF::UNIT_FIELD_LEVEL), 12}}, /*create=*/true);
    CHECK(h.game().player().fields().find(uf(UF::PLAYER_XP)) == h.game().player().fields().end());
}

TEST_CASE("a form's bonus action bar is the one SpellShapeshiftForm.dbc gives it",
          "[player][harness][tables]") {
    GameHarness h;
    // Bear form (5) swaps in bar 8, cat form (1) bar 7; travel form (3) none.
    h.dbc("SpellShapeshiftForm.dbc", 35, {{{0, 1}, {1, 7}}, {{0, 3}, {1, 0}}, {{0, 5}, {1, 8}}});
    h.enterWorld();
    h.playerInView();

    CHECK(h.game().getBonusActionBarOffset() == 0);
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 5u << 24}});
    CHECK(h.game().getBonusActionBarOffset() == 8);
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 1u << 24}});
    CHECK(h.game().getBonusActionBarOffset() == 7);
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 3u << 24}});
    CHECK(h.game().getBonusActionBarOffset() == 0);
}

TEST_CASE("a stand state the server confirms is announced once; the next character stands",
          "[player][harness]") {
    namespace ev = wowee::game::events;
    GameHarness h;
    h.enterWorld();
    std::vector<int> states;
    auto sub = h.events().subscribe<ev::StandStateChanged>(
        [&](const ev::StandStateChanged& e) { states.push_back(e.standState); });

    // SMSG_STANDSTATE_UPDATE: sit, sit again, stand, sit.
    for (uint8_t s : {1, 1, 0, 1}) h.receive(Opcode::SMSG_STANDSTATE_UPDATE, Body().u8(s));
    h.pump();
    CHECK(states == std::vector<int>{1, 0, 1});
    CHECK(h.game().player().standState() == 1);

    // Logged out seated. The next character stands until told otherwise, so
    // its server's "sit" is news.
    PlayerSpec bob;
    bob.guid = 0x11;
    bob.name = "Bob";
    h.relogAs(bob);
    CHECK(h.game().player().standState() == 0);
    h.receive(Opcode::SMSG_STANDSTATE_UPDATE, Body().u8(1));
    h.pump();
    CHECK(states == std::vector<int>{1, 0, 1, 1});
}

TEST_CASE("sitting down is asked of the server, and the player sits when it answers",
          "[player][harness]") {
    GameHarness h;
    h.enterWorld();
    h.game().player().requestStandState(1);

    auto* sent = h.lastSent(Opcode::CMSG_STANDSTATECHANGE);
    REQUIRE(sent);
    CHECK(sent->readUInt32() == 1u);
    CHECK(h.game().player().standState() == 0);
}

TEST_CASE("the stun flag on the player is a stun, except as the logout's root",
          "[player][harness]") {
    namespace ev = wowee::game::events;
    constexpr uint32_t kStunned = 0x00040000;
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    std::vector<bool> stuns;
    auto sub = h.events().subscribe<ev::StunStateChanged>(
        [&](const ev::StunStateChanged& e) { stuns.push_back(e.stunned); });

    h.playerFields({{uf(UF::UNIT_FIELD_FLAGS), kStunned}});
    h.playerFields({{uf(UF::UNIT_FIELD_FLAGS), kStunned | 0x1}});   // still stunned: no news
    h.playerFields({{uf(UF::UNIT_FIELD_FLAGS), 0}});
    CHECK(stuns == std::vector<bool>{true, false});

    // The server roots the player for the logout countdown with the same flag.
    h.game().social().requestLogout(false);
    h.playerFields({{uf(UF::UNIT_FIELD_FLAGS), kStunned}});
    CHECK(stuns == std::vector<bool>{true, false});
}

TEST_CASE("the player is stealthed while the creep flag is on", "[player][harness]") {
    namespace ev = wowee::game::events;
    constexpr uint32_t kCreep = 0x02u << 16;   // the visibility byte of UNIT_FIELD_BYTES_1
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    std::vector<bool> stealth;
    auto sub = h.events().subscribe<ev::StealthStateChanged>(
        [&](const ev::StealthStateChanged& e) { stealth.push_back(e.stealthed); });

    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), kCreep}});
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), kCreep | 1}});   // stand state moved, not stealth
    h.playerFields({{uf(UF::UNIT_FIELD_BYTES_1), 0}});
    CHECK(stealth == std::vector<bool>{true, false});
}
