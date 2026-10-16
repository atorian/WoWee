// The action bars the realm keeps for a character: which of the four extra
// bars are shown arrives with the character and goes back when it changes.

#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "test_support.hpp"
#include "game/action_bar_handler.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"

#include <memory>

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
