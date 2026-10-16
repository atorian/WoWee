// The action bars the realm keeps for a character: which of the four extra
// bars are shown arrives with the character and goes back when it changes,
// and what a button holds follows the spell book - its cooldown, its rank.

#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "test_support.hpp"
#include "game/action_bar_handler.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"

#include <memory>
#include <string>
#include <vector>

using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::game::Opcode;

namespace {

/// SMSG_UPDATE_OBJECT carrying one VALUES block that sets one field.
std::vector<uint8_t> valuesUpdate(uint64_t guid, uint16_t field, uint32_t value) {
    Body b;
    b.u32(1);   // one block
    b.u8(0);    // VALUES
    // Packed guid: a mask of the non-zero bytes, then those bytes.
    uint8_t mask = 0;
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 8; ++i) {
        const auto byte = static_cast<uint8_t>(guid >> (i * 8));
        if (byte != 0) {
            mask = static_cast<uint8_t>(mask | (1u << i));
            bytes.push_back(byte);
        }
    }
    b.u8(mask);
    for (uint8_t byte : bytes) b.u8(byte);
    const int words = field / 32 + 1;
    b.u8(static_cast<uint8_t>(words));
    for (int w = 0; w < words; ++w) {
        b.u32(w == field / 32 ? (1u << (field % 32)) : 0u);
    }
    b.u32(value);
    return b.bytes();
}

} // namespace

TEST_CASE("the extra action bars the realm keeps shown are the ones the character has",
          "[actionbar][harness]") {
    GameHarness h;
    h.enterWorld();
    const uint64_t guid = h.player().guid;
    h.game().getEntityManager().addEntity(guid, std::make_shared<wowee::game::Player>(guid));
    auto& bars = h.game().actionBar();

    // Before the realm has said: not known, rather than "none shown".
    CHECK_FALSE(bars.extraBarsShown().has_value());

    // Bottom left and right shown - byte 2 of PLAYER_FIELD_BYTES, as the realm
    // sends it with the character.
    const uint16_t field = wowee::game::fieldIndex(wowee::game::UF::PLAYER_FIELD_BYTES);
    REQUIRE(field != 0xFFFF);
    h.receive(Opcode::SMSG_UPDATE_OBJECT, valuesUpdate(guid, field, 0x00050000u));
    h.pump();
    REQUIRE(bars.extraBarsShown().has_value());
    CHECK(*bars.extraBarsShown() == 0x05);

    // The player shows the bottom right bar instead: the realm is told, and the
    // answer is the new one before it echoes it.
    bars.setExtraBarsShown(0x02);
    auto* sent = h.lastSent(Opcode::CMSG_SET_ACTIONBAR_TOGGLES);
    REQUIRE(sent);
    CHECK(sent->readUInt8() == 0x02);
    CHECK(*bars.extraBarsShown() == 0x02);
}

TEST_CASE("a spell already cooling down shows its cooldown on whatever button it is put on",
          "[actionbar][spell][harness]") {
    constexpr uint32_t kFrostNova = 122;
    GameHarness h;
    h.enterWorld();
    auto& bars = h.game().actionBar();

    // SMSG_SPELL_COOLDOWN, 3.3.5a: the caster, a flags byte, then the spell and
    // its cooldown in milliseconds.
    h.receive(Opcode::SMSG_SPELL_COOLDOWN, Body().u64(h.player().guid).u8(0).u32(kFrostNova).u32(25000));
    h.pump();

    // Dragged onto a button after the cooldown began. The button kept a copy
    // of each cooldown that only the spell packets wrote, so this one read as
    // ready and the swirl never drew.
    bars.setSlot(3, wowee::game::ActionBarSlot::SPELL, kFrostNova);
    const auto cooling = bars.cooldown(3);
    CHECK_FALSE(cooling.ready());
    CHECK(cooling.total == Catch::Approx(25.0f));
    CHECK(cooling.remaining > 24.0f);
    CHECK(bars.cooldown(4).ready());   // an empty button has none

    // The server says it is over: every button holding it is ready at once.
    h.receive(Opcode::SMSG_COOLDOWN_EVENT, Body().u32(kFrostNova).u64(h.player().guid));
    h.pump();
    CHECK(bars.cooldown(3).ready());
}

TEST_CASE("a spell's next rank takes its place on the buttons, and an unlearned one leaves them",
          "[actionbar][spell][harness]") {
    constexpr uint32_t kFireballR1 = 133;
    constexpr uint32_t kFireballR2 = 143;
    constexpr uint32_t kBlink = 1953;
    GameHarness h;
    h.enterWorld();
    auto& bars = h.game().actionBar();
    bars.setSlot(0, wowee::game::ActionBarSlot::SPELL, kFireballR1);
    bars.setSlot(12, wowee::game::ActionBarSlot::SPELL, kFireballR1);
    bars.setSlot(1, wowee::game::ActionBarSlot::SPELL, kBlink);
    h.clearRecords();

    h.receive(Opcode::SMSG_SUPERCEDED_SPELL, Body().u32(kFireballR1).u32(kFireballR2));
    h.pump();
    CHECK(bars.slots()[0].id == kFireballR2);
    CHECK(bars.slots()[12].id == kFireballR2);
    // Not sent: the server already holds the bar with the new rank.
    CHECK(h.countSent(Opcode::CMSG_SET_ACTION_BUTTON) == 0);
    // Every button told to redraw, once - zero is how the event says "all".
    size_t everyButton = 0;
    for (const auto& e : h.addonEvents("ACTIONBAR_SLOT_CHANGED")) {
        if (e.args == std::vector<std::string>{"0"}) ++everyButton;
    }
    CHECK(everyButton == 1);

    h.receive(Opcode::SMSG_REMOVED_SPELL, Body().u32(kBlink));
    h.pump();
    CHECK(bars.slots()[1].isEmpty());
    CHECK(bars.slots()[0].id == kFireballR2);
}
