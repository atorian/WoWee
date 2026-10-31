// Spells, end to end: cooldowns and refused casts from the server's packets to
// what the interface reads and is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/entity.hpp"
#include "game/entity_controller.hpp"
#include "game/game_events.hpp"
#include "game/update_field_table.hpp"

#include <algorithm>
#include <map>
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
    CHECK(h.game().spells().getGCDRemaining() > 0.0f);
    CHECK(h.game().spells().getGCDRemaining() <= 1.5f);
    CHECK_THAT(h.game().spells().getGCDTotal(), WithinAbs(1.5, 1e-3));
}

TEST_CASE("a spell's own cooldown runs down with game time", "[spell][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_SPELL_COOLDOWN, cooldowns(h.player().guid, {{kFireball, 10000}}));
    h.pump();
    CHECK_THAT(h.game().spells().getSpellCooldown(kFireball), WithinAbs(10.0, 0.1));

    h.advance(4.0f);
    CHECK_THAT(h.game().spells().getSpellCooldown(kFireball), WithinAbs(6.0, 0.1));
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
    const auto& learned = h.game().spells().getKnownSpells();
    CHECK(learned.count(133) == 1);
    CHECK(learned.count(8690) == 0); // an item effect was not a learned ability
    CHECK(learned.count(6603) == 0); // neither was an ability absent from this packet
    std::vector<uint32_t> shown;
    for (const auto& tab : h.game().spells().getSpellBookTabs())
        shown.insert(shown.end(), tab.spellIds.begin(), tab.spellIds.end());
    CHECK(shown == std::vector<uint32_t>{133});
}

TEST_CASE("before WotLK the player's auras are the aura fields, harmful ones marked as debuffs",
          "[spell][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.useExpansion("classic");
    h.clearRecords();

    // UNIT_FIELD_AURAS holds a spell id per slot; UNIT_FIELD_AURAFLAGS packs a
    // flag byte per slot, four to a field. Slot 3 is harmful (0x02 before
    // WotLK).
    UpdateBlock block{};
    block.guid = h.player().guid;
    block.objectType = ObjectType::PLAYER;
    block.updateType = UpdateType::VALUES;
    block.fields[fieldIndex(UF::UNIT_FIELD_AURAS) + 0] = 1243;
    block.fields[fieldIndex(UF::UNIT_FIELD_AURAS) + 3] = 172;
    block.fields[fieldIndex(UF::UNIT_FIELD_AURAFLAGS)] = 0x02u << 24;
    UpdateObjectData batch{};
    batch.blocks.push_back(block);
    h.game().entityController().applyRecordedObjects(batch);
    h.pump();

    const auto& auras = h.game().spells().getPlayerAuras();
    REQUIRE(auras.size() >= 4);
    CHECK(auras[0].spellId == 1243);
    CHECK((auras[0].flags & 0x80) == 0);
    CHECK(auras[3].spellId == 172);
    // Normalised to WotLK's debuff bit, which everything downstream reads.
    CHECK((auras[3].flags & 0x80) != 0);
    CHECK((auras[3].flags & 0x02) == 0);
    CHECK(auras[3].casterGuid == h.player().guid);
    CHECK(h.addonEvents("UNIT_AURA").size() == 1);
    CHECK(h.addonEvents("PLAYER_AURAS_CHANGED").size() == 1);
    CHECK(h.addonEvents("MINIMAP_UPDATE_TRACKING").size() == 1);
}

TEST_CASE("a spell's name and rank are the ones Spell.dbc gives it", "[spell][harness][tables]") {
    GameHarness h;
    // The WotLK file: 234 fields, the name at 136 and the rank at 153.
    h.dbc("Spell.dbc", 234, {{{0, kFireball}, {136, "Fireball"}, {153, "Rank 1"}}});
    h.enterWorld();

    CHECK(h.game().spells().getSpellName(kFireball) == "Fireball");
    CHECK(h.game().spells().getSpellRank(kFireball) == "Rank 1");
    CHECK(h.game().spells().getSpellName(kHearthstone).empty());
}

TEST_CASE("the mounts and pets are the known spells Spell.dbc says summon one",
          "[spell][harness][tables]") {
    constexpr uint32_t kBrownHorse = 458, kMechanicalSquirrel = 4055;
    GameHarness h;
    // A mount applies aura 78 (column 95); a critter is effect 28 (column 71)
    // with EffectMiscValueB 41 (column 113). The misc value beside either
    // (column 110) is the creature.
    h.dbc("Spell.dbc", 234, {
        {{0, kBrownHorse}, {136, "Brown Horse"}, {95, 78}, {110, 308}},
        {{0, kMechanicalSquirrel}, {136, "Mechanical Squirrel"}, {71, 28}, {113, 41}, {110, 2671}},
        {{0, kFireball}, {136, "Fireball"}, {71, 2}},
    });
    h.enterWorld();
    // The talent spec, a 16-bit count, each spell with a 16-bit slot, and no
    // cooldowns.
    h.receive(Opcode::SMSG_INITIAL_SPELLS, Body().u8(0).u8(3).u8(0)
                                               .u32(kBrownHorse).u8(0).u8(0)
                                               .u32(kMechanicalSquirrel).u8(0).u8(0)
                                               .u32(kFireball).u8(0).u8(0)
                                               .u8(0).u8(0));
    h.pump();

    const auto& mounts = h.game().getCompanions(true);
    REQUIRE(mounts.size() == 1);
    CHECK(mounts[0].spellId == kBrownHorse);
    CHECK(mounts[0].creatureId == 308);
    const auto& critters = h.game().getCompanions(false);
    REQUIRE(critters.size() == 1);
    CHECK(critters[0].spellId == kMechanicalSquirrel);
    CHECK(critters[0].creatureId == 2671);
}

TEST_CASE("a cast refused for want of a crafting station names the one SpellFocusObject.dbc does",
          "[spell][harness][tables]") {
    GameHarness h;
    h.dbc("SpellFocusObject.dbc", 2, {{{0, 1}, {1, "Anvil"}}, {{0, 3}, {1, "Forge"}}});
    h.enterWorld();

    h.receive(Opcode::SMSG_CAST_FAILED, Body().u8(1).u32(2660).u8(102 /* requires spell focus */).u32(3));
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args.at(0) == "Requires Forge nearby.");
}

TEST_CASE("a cast refused for a missing tool names the TotemCategory.dbc row it asked for",
          "[spell][harness][tables]") {
    GameHarness h;
    h.dbc("TotemCategory.dbc", 2, {{{0, 14}, {1, "Blacksmith Hammer"}}});
    h.enterWorld();

    h.receive(Opcode::SMSG_CAST_FAILED,
              Body().u8(1).u32(2660).u8(130 /* totem category */).u32(14).u32(0));
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args.at(0) == "Requires Blacksmith Hammer in your inventory.");
}

TEST_CASE("a learned talent is placed in the tree Talent.dbc and TalentTab.dbc describe",
          "[spell][harness][tables]") {
    GameHarness h;
    // Talent 31: tab 41, row 2, column 1, three ranks. Tab 41: "Fire", mage.
    h.dbc("Talent.dbc", 23, {{{0, 31}, {1, 41}, {2, 2}, {3, 1}, {4, 11069}, {5, 12338}, {6, 12339}}});
    h.dbc("TalentTab.dbc", 24, {{{0, 41}, {1, "Fire"}, {20, 128}, {22, 1}}});
    h.enterWorld();

    // SMSG_TALENTS_INFO for the player: one group, talent 31 at its first rank.
    h.receive(Opcode::SMSG_TALENTS_INFO,
              Body().u8(0).u32(0).u8(1).u8(0).u8(1).u32(31).u8(0).u8(0));
    h.pump();

    CHECK(h.game().spells().getTalentRank(31) == 1);
    const auto* talent = h.game().spells().getTalentEntry(31);
    REQUIRE(talent != nullptr);
    CHECK(talent->maxRank == 3);
    CHECK(talent->row == 2);
    const auto* tab = h.game().spells().getTalentTabEntry(talent->tabId);
    REQUIRE(tab != nullptr);
    CHECK(tab->name == "Fire");
    CHECK(tab->classMask == 128);
}

TEST_CASE("a profession spell's skill, its name and its heading come from the skill tables",
          "[spell][harness][tables]") {
    constexpr uint32_t kRoughSharpeningStone = 2660;
    GameHarness h;
    h.dbc("SkillLineAbility.dbc", 14, {{{0, 1}, {1, 164}, {2, kRoughSharpeningStone}}});
    h.dbc("SkillLine.dbc", 56, {{{0, 164}, {1, 11}, {3, "Blacksmithing"}}});
    h.dbc("SkillLineCategory.dbc", 19, {{{0, 11}, {1, "Professions"}, {18, 3}}});
    h.enterWorld();

    const auto& spells = h.game().spells();
    CHECK(spells.skillLineOfSpell(kRoughSharpeningStone) == 164);
    CHECK(spells.getSkillLineName(164) == "Blacksmithing");
    CHECK(spells.skillCategory(164) == 11);
    CHECK(spells.skillCategoryName(11) == "Professions");
}

TEST_CASE("an enchantment's name and the gem it came from are SpellItemEnchantment.dbc's",
          "[spell][harness][tables]") {
    GameHarness h;
    // The WotLK file: 38 fields, the name at 14 and the source gem at 33.
    h.dbc("SpellItemEnchantment.dbc", 38,
          {{{0, 2943}, {14, "+20 Strength"}, {33, 40111}}});
    h.enterWorld();

    CHECK(h.game().spells().getEnchantName(2943) == "+20 Strength");
    CHECK(h.game().spells().getEnchantGemItem(2943) == 40111);
    CHECK(h.game().spells().getEnchantName(1).empty());
}

namespace {

constexpr uint64_t kChest = 0xF110000000004004ull;
constexpr uint64_t kVein = 0xF110000000004005ull;

/// SMSG_GAMEOBJECT_QUERY_RESPONSE, 3.3.5a: entry, type, display, four names,
/// three more strings, the twenty-four data fields (data[0] the lock), size.
std::vector<uint8_t> gameObjectInfo(uint32_t entry, uint32_t type, const std::string& name,
                                    uint32_t lockId) {
    Body b;
    b.u32(entry).u32(type).u32(1).cstr(name).cstr("").cstr("").cstr("");
    b.cstr("").cstr("").cstr("");
    b.u32(lockId);
    for (int i = 1; i < 24; ++i) b.u32(0);
    b.f32(1.0f);
    return b.bytes();
}

/// A game object of `entry` at the world's origin, with the player stood
/// beside it and its description known. Each test uses an object of its own:
/// a click on the same one within 150 ms is dropped as a repeat.
void objectBesidePlayer(GameHarness& h, uint64_t guid, uint32_t entry, uint32_t type,
                        const std::string& name, uint32_t lockId) {
    using wowee::game::fieldIndex;
    using wowee::game::UF;
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(guid, wowee::game::ObjectType::GAMEOBJECT,
                                            {{fieldIndex(UF::OBJECT_FIELD_ENTRY), entry}}));
    h.receive(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE, gameObjectInfo(entry, type, name, lockId));
    h.pump();
    h.game().movement().setPosition(1.0f, 0.0f, 0.0f);
}

/// SMSG_INITIAL_SPELLS knowing these spells, with no cooldowns.
std::vector<uint8_t> initialSpells(const std::vector<uint32_t>& spells) {
    Body b;
    b.u8(0).u8(static_cast<uint8_t>(spells.size())).u8(0);
    for (uint32_t id : spells) b.u32(id).u8(0).u8(0);
    b.u8(0).u8(0);
    return b.bytes();
}

}  // namespace

TEST_CASE("a locked chest is opened with the known spell Lock.dbc and Spell.dbc say fits it",
          "[spell][harness][tables]") {
    constexpr uint32_t kPickLock = 1804;
    GameHarness h;
    // Lock 57: one key, a lock type (2) of index 1 - lockpicking.
    h.dbc("Lock.dbc", 33, {{{0, 57}, {1, 2}, {9, 1}, {17, 75}}});
    // Pick Lock: effect 33 (open lock) at 71 with misc value 1 at 110.
    h.dbc("Spell.dbc", 234, {{{0, kPickLock}, {136, "Pick Lock"}, {71, 33}, {110, 1}}});
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIAL_SPELLS, initialSpells({kPickLock}));
    h.pump();
    objectBesidePlayer(h, kChest, 2843, 3 /* chest */, "Battered Chest", 57);
    h.clearRecords();

    h.game().interactWithGameObject(kChest);

    auto* cast = h.lastSent(Opcode::CMSG_CAST_SPELL);
    REQUIRE(cast != nullptr);
    cast->readUInt8();  // cast count
    CHECK(cast->readUInt32() == kPickLock);
    CHECK(h.countSent(Opcode::CMSG_GAMEOBJ_USE) == 0);
}

TEST_CASE("a node refused for want of skill names the rank its Lock.dbc row asks for",
          "[spell][harness][tables]") {
    constexpr uint32_t kMining = 2575;
    GameHarness h;
    // Lock 38: a skill key (2) of lock type 3, mining, at rank 75.
    h.dbc("Lock.dbc", 33, {{{0, 38}, {1, 2}, {9, 3}, {17, 75}}});
    h.enterWorld();
    h.receive(Opcode::SMSG_INITIAL_SPELLS, initialSpells({kMining}));
    h.pump();
    objectBesidePlayer(h, kVein, 1731, 3, "Tin Vein", 38);
    h.game().interactWithGameObject(kVein);
    REQUIRE(h.lastSent(Opcode::CMSG_CAST_SPELL) != nullptr);
    h.clearRecords();

    h.receive(Opcode::SMSG_CAST_FAILED, castFailed(kMining, 49 /* low cast level */));
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args.at(0) == "Requires Mining skill 75.");
}

namespace {
/// The player's UNIT_FIELD_CRITTER, the companion's guid low word first.
std::map<uint16_t, uint32_t> critterFields(uint64_t guid) {
    const auto f = static_cast<uint16_t>(wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_CRITTER));
    return {{f, static_cast<uint32_t>(guid)}, {static_cast<uint16_t>(f + 1), static_cast<uint32_t>(guid >> 32)}};
}
constexpr uint32_t kMechanicalSquirrel = 4055;
constexpr uint64_t kSquirrel = 0xF130000B2A000042ull;
}  // namespace

TEST_CASE("casting the spell that called the companion out puts it away instead",
          "[spells][companion][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.game().spells().noteGroundCastSpell(kMechanicalSquirrel);
    h.playerFields(critterFields(kSquirrel));
    h.clearRecords();

    h.game().spells().castSpell(kMechanicalSquirrel, 0);
    auto* dismiss = h.lastSent(Opcode::CMSG_DISMISS_CRITTER);
    REQUIRE(dismiss);
    CHECK(dismiss->readUInt64() == kSquirrel);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_CAST_SPELL));

    // The server puts it away by clearing the field; the spell summons again.
    h.playerFields(critterFields(0));
    h.clearRecords();
    h.game().spells().castSpell(kMechanicalSquirrel, 0);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_DISMISS_CRITTER));
}

TEST_CASE("the next character has none of the last one's companion out", "[spells][companion][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.game().spells().noteGroundCastSpell(kMechanicalSquirrel);
    h.playerFields(critterFields(kSquirrel));

    PlayerSpec bob;
    bob.guid = 0x11;
    bob.name = "Bob";
    h.relogAs(bob);
    h.game().spells().castSpell(kMechanicalSquirrel, 0);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_DISMISS_CRITTER));
}
