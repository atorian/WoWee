// The inventory, end to end: item objects the realm creates and the slots it
// puts them in, the coin, the loot window, books, refunds and what each tells
// the interface and the client.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/entity.hpp"
#include "game/game_events.hpp"
#include "game/inventory_handler.hpp"
#include "game/update_field_table.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::fieldIndex;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;
namespace ev = wowee::game::events;

namespace {

constexpr uint64_t kCorpse = 0xF130000001000001ull;
constexpr uint64_t kSword = 0x4000000000000A01ull;
constexpr uint64_t kCloth = 0x4000000000000A02ull;

using Fields = GameHarness::Fields;

std::vector<uint8_t> createItem(uint64_t guid, uint32_t entry, uint32_t stack) {
    return GameHarness::createItemBody(guid, entry, stack);
}

std::vector<uint8_t> values(uint64_t guid, const Fields& fields) {
    return GameHarness::valuesBody(guid, fields);
}

/// The player's own fields: an item in a backpack slot.
Fields inBackpack(int slot, uint64_t item) {
    Fields fields;
    GameHarness::putGuid(fields, static_cast<uint16_t>(fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1) + slot * 2),
                         item);
    return fields;
}

/// The player's own fields: an item in the main hand.
Fields inMainHand(uint64_t item) {
    Fields fields;
    GameHarness::putGuid(fields,
                         static_cast<uint16_t>(fieldIndex(UF::PLAYER_FIELD_INV_SLOT_HEAD) +
                                               static_cast<int>(wowee::game::EquipSlot::MAIN_HAND) * 2),
                         item);
    return fields;
}

/// SMSG_LOOT_RESPONSE for a corpse holding money and nothing else, as 3.3.5a
/// writes it: the loot guid, the loot type, the money, no items and no quest
/// items.
std::vector<uint8_t> moneyLoot(uint64_t lootGuid, uint32_t copper) {
    return Body().u64(lootGuid).u8(1 /* corpse */).u32(copper).u8(0).u8(0);
}

/// Every LootMoneyReceived the game announces, while it lives.
struct MoneyAnnounced {
    explicit MoneyAnnounced(GameHarness& h)
        : sub(h.events().subscribe<ev::LootMoneyReceived>(
              [this](const ev::LootMoneyReceived& e) { seen.push_back(e); })) {}
    std::vector<ev::LootMoneyReceived> seen;
    wowee::core::EventBus::Subscription sub;
};

size_t lootedLines(GameHarness& h) {
    size_t n = 0;
    for (const auto& line : h.game().getChatHistory()) {
        if (line.message.rfind("Looted: ", 0) == 0) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("money the server confirms is taken, counted and announced once", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);
    const uint64_t before = h.game().getMoneyCopper();

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 1234));
    h.pump();
    CHECK(h.addonEvents("LOOT_OPENED").size() == 1);
    CHECK(h.countSent(Opcode::CMSG_LOOT_MONEY) == 1);

    h.receive(Opcode::SMSG_LOOT_MONEY_NOTIFY, Body().u32(1234).u8(1));
    h.pump();
    REQUIRE(money.seen.size() == 1);
    CHECK(money.seen[0].copper == 1234);
    CHECK(money.seen[0].lootGuid == kCorpse);
    CHECK(h.game().getMoneyCopper() == before + 1234);
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);

    // The fallback for a server that never confirms must not say it again.
    h.advance(1.0f);
    CHECK(money.seen.size() == 1);
    CHECK(lootedLines(h) == 1);
}

TEST_CASE("money a server never confirms is still announced, once", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 500));
    h.pump();
    CHECK(money.seen.empty());

    h.advance(1.0f);
    REQUIRE(money.seen.size() == 1);
    CHECK(money.seen[0].copper == 500);
    CHECK(lootedLines(h) == 1);
}

TEST_CASE("the same container looted again later is announced again", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 100));
    h.pump();
    h.advance(1.0f);
    h.receive(Opcode::SMSG_LOOT_RELEASE_RESPONSE, Body().u64(kCorpse).u8(1));
    h.pump();
    REQUIRE(money.seen.size() == 1);

    // The cooldown that stops one loot being announced twice runs out. It
    // was set in InventoryHandler and counted down in a GameHandler copy
    // nothing set, so it never ran out and this container stayed silent for
    // the rest of the session.
    h.advance(2.0f);
    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 100));
    h.pump();
    h.advance(1.0f);
    CHECK(money.seen.size() == 2);
}

TEST_CASE("the loot window opening and closing reaches the client", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<bool> seen;
    auto sub = h.events().subscribe<ev::LootWindowChanged>(
        [&](const ev::LootWindowChanged& e) { seen.push_back(e.open); });

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 100));
    h.pump();
    REQUIRE(seen == std::vector<bool>{true});
    CHECK(h.game().inventory().isLootWindowOpen());

    h.game().inventory().closeLoot();
    CHECK(seen == std::vector<bool>{true, false});
    CHECK(h.lastSent(Opcode::CMSG_LOOT_RELEASE));
}

TEST_CASE("an item the realm puts in the backpack is in the bags the interface reads",
          "[inventory][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    auto& inventory = h.game().inventory();

    h.receive(Opcode::SMSG_UPDATE_OBJECT, createItem(kCloth, 2589, 7));
    h.pump();
    // Its template is asked for, under the item's own guid.
    auto* query = h.lastSent(Opcode::CMSG_ITEM_QUERY_SINGLE);
    REQUIRE(query);
    CHECK(query->readUInt32() == 2589);
    CHECK(query->readUInt64() == kCloth);
    CHECK(inventory.holdsItemObject(kCloth));

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(h.player().guid, inBackpack(0, kCloth)));
    h.pump();
    const auto& slot = inventory.items().getBackpackSlot(0);
    REQUIRE_FALSE(slot.empty());
    CHECK(slot.item.itemId == 2589);
    CHECK(slot.item.stackCount == 7);
    CHECK(inventory.getBackpackItemGuid(0) == kCloth);
    // The backpack's own redraw: the bag events carry the bag they are for.
    bool backpackTold = false;
    for (const auto& e : h.addonEvents("BAG_UPDATE")) {
        if (!e.args.empty() && e.args[0] == "0") backpackTold = true;
    }
    CHECK(backpackTold);

    // A stack used down to three is three in the bag.
    Fields stack{{fieldIndex(UF::ITEM_FIELD_STACK_COUNT), 3}};
    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kCloth, stack));
    h.pump();
    CHECK(inventory.items().getBackpackSlot(0).item.stackCount == 3);

    // Destroyed, it is gone from the slot that held it.
    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(kCloth).u8(0));
    h.pump();
    CHECK_FALSE(inventory.holdsItemObject(kCloth));
    CHECK(inventory.items().getBackpackSlot(0).empty());
}

TEST_CASE("the next character holds nothing the last one held", "[inventory][harness]") {
    PlayerSpec alice;
    alice.guid = 0x21;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x22;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.playerInView();
    auto& inventory = h.game().inventory();

    // Alice: a sword in hand with a poison on it, a stack in the backpack and
    // some coin.
    h.receive(Opcode::SMSG_UPDATE_OBJECT, createItem(kSword, 25, 1));
    h.receive(Opcode::SMSG_UPDATE_OBJECT, createItem(kCloth, 2589, 7));
    auto fields = inMainHand(kSword);
    for (const auto& [field, value] : inBackpack(0, kCloth)) fields[field] = value;
    fields[fieldIndex(UF::PLAYER_FIELD_COINAGE)] = 12345;
    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(alice.guid, fields));
    h.pump();
    h.receive(Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE,
              Body().u64(kSword).u32(1).u32(1800).u64(alice.guid));
    h.pump();
    REQUIRE(inventory.getEquipSlotGuid(static_cast<int>(wowee::game::EquipSlot::MAIN_HAND)) == kSword);
    REQUIRE_FALSE(inventory.items().getBackpackSlot(0).empty());
    REQUIRE(h.game().getMoneyCopper() == 12345);
    REQUIRE(h.game().getTempEnchantRemainingMs(0) > 0);

    h.relogAs(bob);

    // Bob's bags, coin and weapons are his own, and the realm has said
    // nothing of them yet.
    CHECK(inventory.items().getBackpackSlot(0).empty());
    CHECK(inventory.getEquipSlotGuid(static_cast<int>(wowee::game::EquipSlot::MAIN_HAND)) == 0);
    CHECK_FALSE(inventory.holdsItemObject(kSword));
    CHECK(h.game().getMoneyCopper() == 0);
    // The poison was on Alice's sword. It stayed on the list the weapon
    // enchant timer reads, and Bob's main hand showed it running down.
    CHECK(h.game().getTempEnchantRemainingMs(0) == 0);
}

TEST_CASE("a book's pages are followed to the last, and the reader is told of each",
          "[inventory][book][harness]") {
    GameHarness h;
    h.enterWorld();
    auto& inventory = h.game().inventory();
    inventory.beginBook("The Old Tome", 2);

    h.receive(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, Body().u32(100).cstr("Once upon").u32(101));
    h.pump();
    // The next page is asked for.
    auto* next = h.lastSent(Opcode::CMSG_PAGE_TEXT_QUERY);
    REQUIRE(next);
    CHECK(next->readUInt32() == 101);
    CHECK(h.addonEvents("ITEM_TEXT_BEGIN").size() == 1);
    CHECK(h.addonEvents("ITEM_TEXT_READY").size() == 1);

    h.clearRecords();
    h.receive(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, Body().u32(101).cstr("a time.").u32(0));
    h.pump();
    CHECK(h.countSent(Opcode::CMSG_PAGE_TEXT_QUERY) == 0);
    CHECK(h.addonEvents("ITEM_TEXT_BEGIN").empty());
    CHECK(h.addonEvents("ITEM_TEXT_READY").size() == 1);

    const auto& pages = inventory.getBookPages();
    REQUIRE(pages.size() == 2);
    CHECK(pages[0].text == "Once upon");
    CHECK(pages[1].text == "a time.");
    CHECK(h.game().getBookTitle() == "The Old Tome");
    CHECK(h.game().getBookMaterial() == 2);

    // A chain that loops back is not asked for again.
    h.receive(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, Body().u32(101).cstr("a time.").u32(100));
    h.pump();
    CHECK(h.countSent(Opcode::CMSG_PAGE_TEXT_QUERY) == 0);
    CHECK(inventory.getBookPages().size() == 2);
}

TEST_CASE("what an item cost is asked for once and kept for the interface",
          "[inventory][refund][harness]") {
    GameHarness h;
    h.enterWorld();
    auto& inventory = h.game().inventory();

    inventory.requestItemRefundInfo(kSword);
    inventory.requestItemRefundInfo(kSword);
    REQUIRE(h.countSent(Opcode::CMSG_ITEM_REFUND_INFO) == 1);
    CHECK(h.lastSent(Opcode::CMSG_ITEM_REFUND_INFO)->readUInt64() == kSword);

    Body reply;
    reply.u64(kSword).u32(5000).u32(75).u32(0);
    reply.u32(29434).u32(2);                          // two badges
    for (int i = 0; i < 4; ++i) reply.u32(0).u32(0);
    reply.u32(0).u32(600);                            // ten played minutes ago
    h.receive(Opcode::SMSG_ITEM_REFUND_INFO_RESPONSE, reply);
    h.pump();

    const auto* refund = inventory.getItemRefundInfo(kSword);
    REQUIRE(refund);
    CHECK(refund->money == 5000);
    CHECK(refund->honor == 75);
    CHECK(refund->items[0].first == 29434);
    CHECK(refund->items[0].second == 2);
    CHECK(refund->playedSincePurchase == 600);
    CHECK(h.addonEvents("UPDATE_INVENTORY_ALERTS").size() == 1);
}

TEST_CASE("a vendor's window opening is the vendor's greeting, from where the vendor stands",
          "[inventory][vendor][harness]") {
    GameHarness h;
    h.enterWorld();
    constexpr uint64_t kVendor = 0xF130000000000077ull;
    // A step from the player: a vendor out of reach closes its own window.
    const auto& at = h.game().getMovementInfo();
    const float vx = at.x + 2.0f, vy = at.y + 1.0f, vz = at.z;
    auto vendor = std::make_shared<wowee::game::Unit>(kVendor);
    vendor->setPosition(vx, vy, vz, 0.0f);
    h.game().getEntityManager().addEntity(kVendor, vendor);
    std::vector<ev::VendorOpened> opened;
    auto sub = h.events().subscribe<ev::VendorOpened>(
        [&](const ev::VendorOpened& e) { opened.push_back(e); });

    h.receive(Opcode::SMSG_LIST_INVENTORY, Body().u64(kVendor).u8(0));
    h.pump();

    CHECK(h.game().inventory().isVendorWindowOpen());
    CHECK(h.addonEvents("MERCHANT_SHOW").size() == 1);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].vendorGuid == kVendor);
    CHECK(opened[0].x == vx);
    CHECK(opened[0].y == vy);
    CHECK(opened[0].z == vz);
}

TEST_CASE("a spell bought from a trainer is in the spell book when the interface looks",
          "[inventory][trainer][harness]") {
    GameHarness h;
    h.enterWorld();
    constexpr uint32_t kSpell = 133;  // Fireball
    REQUIRE(h.game().getKnownSpells().count(kSpell) == 0);

    h.receive(Opcode::SMSG_TRAINER_BUY_SUCCEEDED, Body().u64(0xF130000000000088ull).u32(kSpell));
    h.pump();

    CHECK(h.game().getKnownSpells().count(kSpell) == 1);
    CHECK(h.addonEvents("SPELLS_CHANGED").size() == 1);
    CHECK(h.addonEvents("TRAINER_UPDATE").size() == 1);
}

// The glow on a weapon comes from the enchant on it. Another player's
// enchants arrive in their visible-item fields, beside each item's entry -
// 3.3.5a's PLAYER_VISIBLE_ITEM_n_ENCHANTMENT, the permanent enchant in the
// low half and the temporary one in the high - and the client is told them
// with what the player wears. It was told the items alone, so nobody else's
// weapon ever glowed.
namespace {

constexpr uint64_t kOtherPlayer = 0x0000000000000777ull;
constexpr uint32_t kBlade = 19019;
constexpr uint32_t kBladeDisplay = 30606;
constexpr uint32_t kCrusader = 1900;       // permanent: a weapon enchant
constexpr uint32_t kInstantPoison = 323;   // temporary: a poison on the blade
constexpr int kMainHand = static_cast<int>(wowee::game::EquipSlot::MAIN_HAND);
// 3.3.5a's PLAYER_VISIBLE_ITEM_1_ENTRYID; each slot is an entry and an
// enchantment field.
constexpr uint16_t kVisibleItem1 = 283;
constexpr uint16_t kMainHandEntryField = kVisibleItem1 + kMainHand * 2;
constexpr uint16_t kMainHandEnchantField = kMainHandEntryField + 1;

/// The last time the client was told what this player wears.
struct WornAnnounced {
    explicit WornAnnounced(GameHarness& h)
        : sub(h.events().subscribe<ev::PlayerEquipmentResolved>(
              [this](const ev::PlayerEquipmentResolved& e) {
                  if (e.guid == kOtherPlayer) last = e;
              })) {}
    std::optional<ev::PlayerEquipmentResolved> last;
    wowee::core::EventBus::Subscription sub;
};

}  // namespace

TEST_CASE("another player's weapon shows the enchant their visible-item fields carry",
          "[inventory][enchant][harness]") {
    GameHarness h;
    h.enterWorld();
    WornAnnounced worn(h);

    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kOtherPlayer, wowee::game::ObjectType::PLAYER,
                                            {{fieldIndex(UF::UNIT_FIELD_HEALTH), 100},
                                             {fieldIndex(UF::UNIT_FIELD_MAXHEALTH), 100},
                                             {kMainHandEntryField, kBlade},
                                             {kMainHandEnchantField, kCrusader}},
                                            h.player().x + 5.0f, h.player().y, h.player().z));
    h.pump();
    // The blade is known by its entry until its template comes back.
    h.receive(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
              GameHarness::itemTemplateBody(kBlade, "Test Blade", kBladeDisplay, 13));
    h.pump();

    REQUIRE(worn.last);
    CHECK(worn.last->displayIds[kMainHand] == kBladeDisplay);
    CHECK(worn.last->enchantIds[kMainHand] == kCrusader);

    // A poison put on the blade shows over the enchant...
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::valuesBody(kOtherPlayer,
                                      {{kMainHandEnchantField, kCrusader | (kInstantPoison << 16)}}));
    h.pump();
    CHECK(worn.last->enchantIds[kMainHand] == kInstantPoison);
    CHECK(worn.last->displayIds[kMainHand] == kBladeDisplay);

    // ...and when it wears off the enchant is back.
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::valuesBody(kOtherPlayer, {{kMainHandEnchantField, kCrusader}}));
    h.pump();
    CHECK(worn.last->enchantIds[kMainHand] == kCrusader);
}

TEST_CASE("the player's own weapon shows a poison over its enchant, and the enchant once it is gone",
          "[inventory][enchant][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    auto& inventory = h.game().inventory();

    // 3.3.5a's ITEM_FIELD_ENCHANTMENT_1_1 and _2_1: the permanent enchant
    // slot's id and the temporary one's.
    constexpr uint16_t kPermanentEnchantField = 22;
    constexpr uint16_t kTemporaryEnchantField = 25;

    h.receive(Opcode::SMSG_UPDATE_OBJECT, createItem(kSword, 25, 1));
    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kSword, {{kPermanentEnchantField, kCrusader}}));
    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(h.player().guid, inMainHand(kSword)));
    h.pump();
    const uint64_t held = inventory.getEquipSlotGuid(kMainHand);
    REQUIRE(held == kSword);
    CHECK(inventory.shownEnchantId(held) == kCrusader);

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kSword, {{kTemporaryEnchantField, kInstantPoison}}));
    h.pump();
    CHECK(inventory.shownEnchantId(held) == kInstantPoison);

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kSword, {{kTemporaryEnchantField, 0}}));
    h.pump();
    CHECK(inventory.shownEnchantId(held) == kCrusader);
}
