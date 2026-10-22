// Spells, end to end: cooldowns and refused casts from the server's packets to
// what the interface reads and is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_events.hpp"

#include <algorithm>
#include <vector>

using Catch::Matchers::WithinAbs;
using wowee::game::Opcode;
using wowee::game::Refusal;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;
namespace ev = wowee::game::events;

namespace {

/// SMSG_SPELL_COOLDOWN as 3.3.5a writes it: the caster, a flags byte, then a
/// spell and its cooldown in milliseconds per entry. Spell 0 is the global
/// cooldown.
std::vector<uint8_t> cooldowns(uint64_t caster, const std::vector<std::pair<uint32_t, uint32_t>>& entries) {
    Body b;
    b.u64(caster).u8(0);
    for (const auto& [spellId, ms] : entries) b.u32(spellId).u32(ms);
    return b.bytes();
}

/// SMSG_CAST_FAILED: cast count, spell, result.
std::vector<uint8_t> castFailed(uint32_t spellId, uint8_t result) {
    return Body().u8(1).u32(spellId).u8(result);
}

/// SMSG_SPELL_START as 3.3.5a writes it: the caster's item (here the caster
/// itself) and the caster, packed; a cast count, the spell, the flags and the
/// cast time; then the targets - a unit, or none.
std::vector<uint8_t> spellStart(uint64_t caster, uint32_t spellId, uint32_t castMs,
                                uint64_t target = 0) {
    Body b;
    b.packedGuid(caster).packedGuid(caster).u8(1).u32(spellId).u32(0).u32(castMs);
    if (target != 0) b.u32(0x0002 /* unit */).packedGuid(target);
    else b.u32(0);
    return b.bytes();
}

/// SMSG_TOTEM_CREATED, 3.3.5a: the slot, the totem (packed), how long it
/// stands in milliseconds, and the spell that put it down.
std::vector<uint8_t> totemCreated(uint8_t slot, uint64_t totem, uint32_t ms, uint32_t spellId) {
    return Body().u8(slot).packedGuid(totem).u32(ms).u32(spellId);
}

constexpr uint32_t kFireball = 133;
constexpr uint32_t kHearthstone = 8690;
constexpr uint32_t kSearingTotem = 3599;
constexpr uint64_t kTotem = 0xF130000000001001ull;
constexpr uint64_t kCultist = 0xF130000000002002ull;

using Op = wowee::game::SpellModOp;

}  // namespace

TEST_CASE("the server's global cooldown is what the interface reads", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();
    REQUIRE_FALSE(h.game().isGCDActive());

    h.receive(Opcode::SMSG_SPELL_COOLDOWN, cooldowns(h.player().guid, {{0, 1500}}));
    h.pump();

    // GetSpellCooldown and the action buttons ask GameHandler. It read a copy
    // of the GCD that SpellHandler never wrote, so this was always zero and
    // the sweep over every button never showed.
    CHECK(h.game().isGCDActive());
    CHECK(h.game().getGCDRemaining() > 0.0f);
    CHECK(h.game().getGCDRemaining() <= 1.5f);
    CHECK_THAT(h.game().getGCDTotal(), WithinAbs(1.5, 1e-3));
}

TEST_CASE("a spell's own cooldown runs down with game time", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_SPELL_COOLDOWN, cooldowns(h.player().guid, {{kFireball, 10000}}));
    h.pump();
    CHECK_THAT(h.game().getSpellCooldown(kFireball), WithinAbs(10.0, 0.1));

    h.advance(4.0f);
    CHECK_THAT(h.game().getSpellCooldown(kFireball), WithinAbs(6.0, 0.1));
    // The global cooldown is not a spell's, and the server sent none.
    CHECK_FALSE(h.game().isGCDActive());
}

TEST_CASE("a refused cast is shown, said aloud, and ends the cast for the interface",
          "[spell][harness]") {
    GameHarness h;
    h.enterWorld();

    std::vector<std::string> errorLines;
    std::vector<Refusal> spoken;
    auto errors = h.events().subscribe<ev::UiErrorRaised>(
        [&](const ev::UiErrorRaised& e) { errorLines.push_back(e.message); });
    auto refusals = h.events().subscribe<ev::PlayerRefused>(
        [&](const ev::PlayerRefused& e) { spoken.push_back(e.reason); });

    h.receive(Opcode::SMSG_CAST_FAILED, castFailed(kFireball, 12 /* invalid target */));
    h.pump();

    REQUIRE(errorLines.size() == 1);
    REQUIRE(h.addonEvents("UI_ERROR_MESSAGE").size() == 1);
    CHECK(h.addonEvents("UI_ERROR_MESSAGE")[0].args.at(0) == errorLines[0]);
    REQUIRE(spoken.size() == 1);
    CHECK(spoken[0] == Refusal::InvalidAttackTarget);

    const auto failed = h.addonEvents("UNIT_SPELLCAST_FAILED");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].args.at(0) == "player");
    CHECK(h.addonEvents("UNIT_SPELLCAST_STOP").size() == 1);
}

TEST_CASE("a talent's modifier lands on the property the spellbook reads", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();

    // SMSG_SET_FLAT_SPELL_MODIFIER / _PCT_: group, op, value. The ops are
    // AzerothCore's SPELLMOD_* numbers: 14 is a spell's cost, 10 its casting
    // time and 11 its cooldown - which must not be read as either.
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(14).u32(static_cast<uint32_t>(-10)));
    h.receive(Opcode::SMSG_SET_PCT_SPELL_MODIFIER, Body().u8(0).u8(10).u32(static_cast<uint32_t>(-20)));
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(1).u8(11).u32(5000));
    h.pump();

    const auto& spells = h.game().spells();
    CHECK(spells.spellFlatMod(Op::Cost) == -10);
    CHECK(spells.spellPctMod(Op::CastingTime) == -20);
    CHECK(spells.spellFlatMod(Op::CastingTime) == 0);
    CHECK(spells.spellPctMod(Op::Cost) == 0);
}

TEST_CASE("a modifier past the ops this client keeps changes nothing", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();

    // Op 40 has no slot; written anyway it would land past the end of the
    // array the modifiers are kept in.
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(40).u32(99));
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(14).u32(7));
    h.pump();

    // The packet after it is still read.
    CHECK(h.game().spells().spellFlatMod(Op::Cost) == 7);
}

TEST_CASE("the next character has none of the last one's totems or talents", "[spell][harness]") {
    PlayerSpec shaman;
    shaman.guid = 0x41;
    shaman.name = "Shaman";
    shaman.classId = 7;
    PlayerSpec warrior;
    warrior.guid = 0x42;
    warrior.name = "Warrior";

    GameHarness h;
    h.enterWorld(shaman);
    h.receive(Opcode::SMSG_TOTEM_CREATED, totemCreated(1, kTotem, 60000, kSearingTotem));
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(14).u32(static_cast<uint32_t>(-10)));
    h.pump();
    REQUIRE(h.game().spells().totemSlot(1).active());
    REQUIRE(h.game().spells().spellFlatMod(Op::Cost) == -10);

    h.relogAs(warrior);

    // The totem slots were the one part of this the character change did not
    // clear: a warrior logged in within a minute had the shaman's Searing
    // Totem standing on the totem bar.
    CHECK_FALSE(h.game().spells().totemSlot(1).active());
    CHECK(h.game().spells().totemSlot(1).spellId == 0);
    CHECK(h.game().spells().spellFlatMod(Op::Cost) == 0);
}

TEST_CASE("the hearthstone's cast says where home is, once the realm has", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::HearthstoneCastStarted> started;
    auto sub = h.events().subscribe<ev::HearthstoneCastStarted>(
        [&](const ev::HearthstoneCastStarted& e) { started.push_back(e); });

    // No home yet: nothing to load ahead of the cast.
    h.receive(Opcode::SMSG_SPELL_START, spellStart(h.player().guid, kHearthstone, 10000));
    h.pump();
    CHECK(started.empty());
    CHECK(h.game().spells().isCasting());

    // SMSG_BINDPOINTUPDATE: server x, y, z, then map and zone.
    h.receive(Opcode::SMSG_BINDPOINTUPDATE,
              Body().f32(1630.0f).f32(-4420.0f).f32(16.0f).u32(1).u32(1637));
    h.receive(Opcode::SMSG_SPELL_START, spellStart(h.player().guid, kHearthstone, 10000));
    h.pump();

    REQUIRE(started.size() == 1);
    CHECK(started[0].mapId == 1);
    // Canonical: the server's x and y swapped.
    CHECK_THAT(started[0].x, WithinAbs(-4420.0, 1e-3));
    CHECK_THAT(started[0].y, WithinAbs(1630.0, 1e-3));
    CHECK_THAT(started[0].z, WithinAbs(16.0, 1e-3));
}

TEST_CASE("another unit's cast is animated from start to its end", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::CastAnimationChanged> seen;
    auto sub = h.events().subscribe<ev::CastAnimationChanged>(
        [&](const ev::CastAnimationChanged& e) { seen.push_back(e); });

    h.receive(Opcode::SMSG_SPELL_START, spellStart(kCultist, kFireball, 3000, h.player().guid));
    h.pump();

    REQUIRE(seen.size() == 1);
    CHECK(seen[0].casterGuid == kCultist);
    CHECK(seen[0].started);
    CHECK_FALSE(seen[0].channel);
    CHECK(seen[0].castType == wowee::game::SpellCastType::DIRECTED);
    CHECK(h.game().spells().getUnitCastState(kCultist) != nullptr);
}

TEST_CASE("an achievement the player earns is kept and announced, another's is not",
          "[spell][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::AchievementEarned> earned;
    auto sub = h.events().subscribe<ev::AchievementEarned>(
        [&](const ev::AchievementEarned& e) { earned.push_back(e); });

    // SMSG_ACHIEVEMENT_EARNED: who (packed), the achievement, the packed date.
    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED, Body().packedGuid(kCultist).u32(6).u32(0x0A1B2C3D));
    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED,
              Body().packedGuid(h.player().guid).u32(7).u32(0x0A1B2C3D));
    h.pump();

    REQUIRE(earned.size() == 1);
    CHECK(earned[0].achievementId == 7);
    CHECK(h.game().getEarnedAchievements().count(7) == 1);
    CHECK(h.game().getEarnedAchievements().count(6) == 0);
    CHECK(h.game().getAchievementDate(7) == 0x0A1B2C3D);
    CHECK(h.addonEvents("ACHIEVEMENT_EARNED").size() == 1);
}

TEST_CASE("a cast the server will not explain is still refused on screen", "[spell][harness]") {
    // Result 27 is DONT_REPORT: the server's way of refusing without a reason.
    // The player still pressed the key and still needs to know it did nothing.
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_CAST_FAILED, castFailed(kFireball, 27));
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args.at(0) == "You can't do that right now");
}

TEST_CASE("a fishing cast with nowhere to land is not answered 'no target'", "[spell][harness]") {
    // Fishing has no unit target: the server picks a point in the water. A
    // "bad targets" refusal there means no water, and the character saying
    // "I have no target" contradicts the fishing hint on screen. The same
    // refusal for an aimed spell is said.
    GameHarness h;
    h.enterWorld();
    std::vector<Refusal> spoken;
    auto refusals = h.events().subscribe<ev::PlayerRefused>(
        [&](const ev::PlayerRefused& e) { spoken.push_back(e.reason); });

    for (uint32_t fishing : {7620u, 51294u}) {   // the first and the last rank
        h.receive(Opcode::SMSG_CAST_FAILED, castFailed(fishing, 11 /* bad targets */));
        h.pump();
    }
    CHECK(spoken.empty());
    CHECK(h.addonEvents("UI_ERROR_MESSAGE").size() == 2);

    h.receive(Opcode::SMSG_CAST_FAILED, castFailed(kFireball, 11));
    h.pump();
    CHECK(spoken == std::vector<Refusal>{Refusal::NoTarget});
}

TEST_CASE("a routine refusal is shown but not said", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();

    size_t errorLines = 0;
    size_t spoken = 0;
    auto errors = h.events().subscribe<ev::UiErrorRaised>([&](const ev::UiErrorRaised&) { ++errorLines; });
    auto refusals = h.events().subscribe<ev::PlayerRefused>([&](const ev::PlayerRefused&) { ++spoken; });

    // Out of range comes back with every press of the key; the character
    // saying so each time would be a chant.
    h.receive(Opcode::SMSG_CAST_FAILED, castFailed(kFireball, 97 /* out of range */));
    h.pump();

    CHECK(errorLines == 1);
    CHECK(spoken == 0);
}

TEST_CASE("rank replacement and unlearning refresh the open spell book", "[spell][harness][book]") {
    GameHarness h;
    h.enterWorld();
    auto& spells = h.game().spells();
    spells.addKnownSpell(133);
    const auto bookIds = [&spells] {
        std::vector<uint32_t> ids;
        for (const auto& tab : spells.getSpellBookTabs())
            ids.insert(ids.end(), tab.spellIds.begin(), tab.spellIds.end());
        return ids;
    };
    const auto before = bookIds();
    REQUIRE(std::find(before.begin(), before.end(), 133) != before.end());
    h.clearRecords();

    h.receive(Opcode::SMSG_SUPERCEDED_SPELL, Body().u32(133).u32(143));
    h.pump();
    const auto after = bookIds();
    CHECK(after.size() == before.size());
    CHECK(std::find(after.begin(), after.end(), 133) == after.end());
    CHECK(std::find(after.begin(), after.end(), 143) != after.end());
    CHECK(h.addonEvents("SPELLS_CHANGED").size() == 1);
    h.clearRecords();

    h.receive(Opcode::SMSG_SEND_UNLEARN_SPELLS, Body().u32(1).u32(143));
    h.pump();
    const auto unlearned = bookIds();
    CHECK(std::find(unlearned.begin(), unlearned.end(), 143) == unlearned.end());
    CHECK(h.addonEvents("SPELLS_CHANGED").size() == 1);
}

TEST_CASE("the initial spell book contains only abilities sent by the realm", "[spell][harness][book]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIAL_SPELLS,
              Body().u8(0).u8(1).u8(0).u32(133).u8(0).u8(0).u8(0).u8(0));
    h.pump();
    const auto& learned = h.game().getKnownSpells();
    CHECK(learned.count(133) == 1);
    CHECK(learned.count(8690) == 0); // an item effect was not a learned ability
    CHECK(learned.count(6603) == 0); // neither was an ability absent from this packet
    std::vector<uint32_t> shown;
    for (const auto& tab : h.game().spells().getSpellBookTabs())
        shown.insert(shown.end(), tab.spellIds.begin(), tab.spellIds.end());
    CHECK(shown == std::vector<uint32_t>{133});
}
