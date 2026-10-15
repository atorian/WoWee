// Spells, end to end: cooldowns and refused casts from the server's packets to
// what the interface reads and is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_events.hpp"

#include <vector>

using Catch::Matchers::WithinAbs;
using wowee::game::Opcode;
using wowee::game::Refusal;
using wowee::test::Body;
using wowee::test::GameHarness;
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

constexpr uint32_t kFireball = 133;

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
    using Op = wowee::game::GameHandler::SpellModOp;
    GameHarness h;
    h.enterWorld();

    // SMSG_SET_FLAT_SPELL_MODIFIER / _PCT_: group, op, value. The ops are
    // AzerothCore's SPELLMOD_* numbers: 14 is a spell's cost, 10 its casting
    // time and 11 its cooldown - which must not be read as either.
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(14).u32(static_cast<uint32_t>(-10)));
    h.receive(Opcode::SMSG_SET_PCT_SPELL_MODIFIER, Body().u8(0).u8(10).u32(static_cast<uint32_t>(-20)));
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(1).u8(11).u32(5000));
    h.pump();

    CHECK(h.game().getSpellFlatMod(Op::Cost) == -10);
    CHECK(h.game().getSpellPctMod(Op::CastingTime) == -20);
    CHECK(h.game().getSpellFlatMod(Op::CastingTime) == 0);
    CHECK(h.game().getSpellPctMod(Op::Cost) == 0);
}

TEST_CASE("a modifier past the ops this client keeps changes nothing", "[spell][harness]") {
    using Op = wowee::game::GameHandler::SpellModOp;
    GameHarness h;
    h.enterWorld();

    // Op 40 has no slot; written anyway it would land past the end of the
    // array the modifiers are kept in.
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(40).u32(99));
    h.receive(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, Body().u8(0).u8(14).u32(7));
    h.pump();

    // The packet after it is still read.
    CHECK(h.game().getSpellFlatMod(Op::Cost) == 7);
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
