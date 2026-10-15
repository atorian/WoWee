#include <catch_amalgamated.hpp>

#include "game/inventory_slots.hpp"

using namespace wowee::game::slots;

// A slot number is what a swap request names. Getting one wrong does not draw
// something odd or answer nil - it moves an item to a real place nobody asked
// for, which looks like the player did it. These pin the figures the client's
// own bank window has been sending, so the interface bindings and that window
// cannot drift apart.

// Vanilla's bank is a different shape from WotLK's, and everything after the
// general slots moves with it. Read off the cores rather than recalled:
// turtle's Player.h has BANK_SLOT_ITEM_START 39 / _END 63, BANK_SLOT_BAG_START
// 63 / _END 69 and KEYRING_SLOT_START 81, and 1.12's own BankFrame.lua says
// NUM_BANKBAGSLOTS = 6 where 3.3.5's constants.lua says 7.
TEST_CASE("Each expansion's bank is where that expansion puts it",
          "[bank][slots]") {
    const BankLayout vanilla = bankLayoutFor(true);
    CHECK(vanilla.generalCount == 24);
    CHECK(vanilla.bagFirst == 63);
    CHECK(vanilla.bagCount == 6);
    CHECK(vanilla.keyringFirst == 81);

    const BankLayout later = bankLayoutFor(false);
    CHECK(later.generalCount == 28);
    CHECK(later.bagFirst == 67);
    CHECK(later.bagCount == 7);
    CHECK(later.keyringFirst == 86);

    // The one thing they agree on, which is why it is still a constant.
    CHECK(bankGeneralWireSlot(0) == 39);
}

TEST_CASE("The interface's first bank bag is the wire's, one higher",
          "[bank][slots]") {
    // BankButtonIDToInvSlotID(1, isBag) answers this, and PickupBagFromSlot is
    // handed it back. The two have to meet exactly or a bag is picked up from
    // one slot and put down in another.
    REQUIRE(firstBankBagInventorySlot() == 68);
    REQUIRE(toWireSlot(firstBankBagInventorySlot()) == bankBagWireSlot(0));
}

TEST_CASE("The backpack and the worn bags are where the wire puts them",
          "[bank][slots]") {
    // Read out of cursorWireSlot and the split path, which have been sending
    // these all along.
    REQUIRE(backpackWireSlot(0) == 23);
    REQUIRE(wornBagContainer(0) == 19);
    // The backpack's slots sit inside no container, so they must not collide
    // with the bank's, which sit there too.
    REQUIRE(backpackWireSlot(kBackpackCount - 1) < bankGeneralWireSlot(0));
}

// ---------------------------------------------------------------------------
// Where the cursor's item came from, which is a slot number like any other and
// went wrong the same way.

TEST_CASE("An item picked up off the action bar names no source slot",
          "[cursor][slots]") {
    // Reported: dragging a food off the action bar into a bag moved the
    // player's equipped bracers into it instead.
    //
    // Every negative source used to mean the paperdoll, and an item action
    // carries a real itemId, so a food lifted off a button was indistinguisha-
    // ble from one lifted off worn equipment - and the only number the pickup
    // had was which button it came from. Button nine less one is eight, which
    // is EquipSlot::WRISTS, so the swap sent named the bracers as its source.
    // Not the ninth button in particular: every button below twenty-four
    // pointed at something worn.
    REQUIRE_FALSE(cursorSourceIsInventory(kCursorNoSource));

    // The three kinds that do name a place, and still do.
    REQUIRE(cursorSourceIsInventory(-1));   // the paperdoll
    REQUIRE(cursorSourceIsInventory(0));    // the backpack
    for (int bag = 1; bag <= kWornBagCount; ++bag) {
        INFO("worn bag " << bag);
        REQUIRE(cursorSourceIsInventory(bag));
    }
}
