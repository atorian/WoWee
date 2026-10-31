// The inventory, end to end: item objects the realm creates and the slots it
// puts them in, the coin, books, refunds and what each tells the interface and
// the client.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/entity.hpp"
#include "game/game_events.hpp"
#include "game/action_bar_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/update_field_table.hpp"

#include <array>
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

}  // namespace

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
    REQUIRE(h.game().inventory().moneyCopper() == 12345);
    REQUIRE(h.game().inventory().getTempEnchantRemainingMs(0) > 0);

    h.relogAs(bob);

    // Bob's bags, coin and weapons are his own, and the realm has said
    // nothing of them yet.
    CHECK(inventory.items().getBackpackSlot(0).empty());
    CHECK(inventory.getEquipSlotGuid(static_cast<int>(wowee::game::EquipSlot::MAIN_HAND)) == 0);
    CHECK_FALSE(inventory.holdsItemObject(kSword));
    CHECK(h.game().inventory().moneyCopper() == 0);
    // The poison was on Alice's sword. It stayed on the list the weapon
    // enchant timer reads, and Bob's main hand showed it running down.
    CHECK(h.game().inventory().getTempEnchantRemainingMs(0) == 0);
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
    CHECK(h.game().inventory().getBookTitle() == "The Old Tome");
    CHECK(h.game().inventory().getBookMaterial() == 2);

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
    REQUIRE(h.game().spells().getKnownSpells().count(kSpell) == 0);

    h.receive(Opcode::SMSG_TRAINER_BUY_SUCCEEDED, Body().u64(0xF130000000000088ull).u32(kSpell));
    h.pump();

    CHECK(h.game().spells().getKnownSpells().count(kSpell) == 1);
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

namespace {
/// The FrameXML events a packet fired, by name, in the order they went out.
std::vector<std::string> eventNames(GameHarness& h) {
    std::vector<std::string> names;
    for (const auto& e : h.addonEvents()) names.push_back(e.name);
    return names;
}

void clothInBackpack(GameHarness& h) {
    h.receive(Opcode::SMSG_UPDATE_OBJECT, createItem(kCloth, 2589, 7));
    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(h.player().guid, inBackpack(0, kCloth)));
    h.pump();
    h.clearRecords();
}
}  // namespace

TEST_CASE("a stack that changes redraws each bag once, then the unit once", "[inventory][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    clothInBackpack(h);

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kCloth, Fields{{fieldIndex(UF::ITEM_FIELD_STACK_COUNT), 3}}));
    h.pump();

    // The backpack, the four worn bags and the seven bank bags, each once.
    std::vector<std::string> bags;
    for (const auto& e : h.addonEvents("BAG_UPDATE")) bags.push_back(e.args.at(0));
    CHECK(bags == std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"});
    CHECK(h.addonEvents("UNIT_INVENTORY_CHANGED").size() == 1);
    CHECK(eventNames(h).back() == "UNIT_INVENTORY_CHANGED");
}

TEST_CASE("an item on the action bar has its buttons recounted when the bags change", "[inventory][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    clothInBackpack(h);
    h.game().actionBar().setSlot(5, wowee::game::ActionBarSlot::ITEM, 2589);
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kCloth, Fields{{fieldIndex(UF::ITEM_FIELD_STACK_COUNT), 2}}));
    h.pump();

    const auto recount = h.addonEvents("ACTIONBAR_SLOT_CHANGED");
    REQUIRE(recount.size() == 1);
    CHECK(recount[0].args.at(0) == "0");
    CHECK(eventNames(h).back() == "ACTIONBAR_SLOT_CHANGED");
}

TEST_CASE("durability that changes is announced to the armour indicator and the repair buttons",
          "[inventory][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    clothInBackpack(h);

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kCloth, Fields{{fieldIndex(UF::ITEM_FIELD_DURABILITY), 20}}));
    h.pump();
    CHECK(h.addonEvents("UPDATE_INVENTORY_ALERTS").size() == 1);
    CHECK(h.addonEvents("UPDATE_INVENTORY_DURABILITY").size() == 1);
    h.clearRecords();

    // A stack count moving is not a durability change.
    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(kCloth, Fields{{fieldIndex(UF::ITEM_FIELD_STACK_COUNT), 5}}));
    h.pump();
    CHECK(h.addonEvents("UPDATE_INVENTORY_ALERTS").empty());
    CHECK(h.addonEvents("UPDATE_INVENTORY_DURABILITY").empty());
}

TEST_CASE("the player's coin is what the money field says, and a change is announced once",
          "[inventory][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.clearRecords();

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(h.player().guid, Fields{{fieldIndex(UF::PLAYER_FIELD_COINAGE), 12345}}));
    h.pump();
    CHECK(h.game().inventory().moneyCopper() == 12345);
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);
    h.clearRecords();

    h.receive(Opcode::SMSG_UPDATE_OBJECT, values(h.player().guid, Fields{{fieldIndex(UF::PLAYER_FIELD_COINAGE), 12345}}));
    h.pump();
    CHECK(h.addonEvents("PLAYER_MONEY").empty());
}

TEST_CASE("a repair costs what DurabilityCosts.dbc and DurabilityQuality.dbc make of the wear",
          "[inventory][harness][tables]") {
    GameHarness h;
    // Item level 60; the sword's column is weapon subclass 7, the file's 8th
    // after the level.
    h.dbc("DurabilityCosts.dbc", 30, {{{0, 59}, {8, 99}}, {{0, 60}, {8, 30}}});
    // Uncommon quality (2) is row (2 + 1) * 2.
    h.dbc("DurabilityQuality.dbc", 2, {{{0, 4}, {1, 0.9f}}, {{0, 6}, {1, 0.5f}}});
    h.enterWorld();

    h.receive(Opcode::SMSG_UPDATE_OBJECT, createItem(kSword, 25, 1));
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              values(kSword, {{fieldIndex(UF::ITEM_FIELD_DURABILITY), 40},
                              {fieldIndex(UF::ITEM_FIELD_MAXDURABILITY), 100}}));
    h.receive(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
              GameHarness::itemTemplateBody(25, "Test Sword", 1000, 13));
    h.pump();

    // Sixty points lost, at 30 copper each, halved for an uncommon item.
    CHECK(h.game().inventory().estimateRepairAllCost() == 900);
}

TEST_CASE("a book's background is the material PageTextMaterial.dbc names", "[inventory][harness][tables]") {
    GameHarness h;
    h.dbc("PageTextMaterial.dbc", 2, {{{0, 1}, {1, "Parchment"}}, {{0, 2}, {1, "Stone"}}});
    h.enterWorld();

    CHECK(h.game().getPageTextMaterialName(2) == "Stone");
    CHECK(h.game().getPageTextMaterialName(1) == "Parchment");
    CHECK(h.game().getPageTextMaterialName(9).empty());
}

namespace {
constexpr uint64_t kSign = 0xF110000400000001ull;
constexpr uint32_t kSignEntry = 1234;
constexpr uint32_t kPage = 55;
constexpr uint32_t kStone = 2;   // PageTextMaterial: the tablet the page is drawn on

void signInView(GameHarness& h) {
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kSign, wowee::game::ObjectType::GAMEOBJECT,
                                            {{static_cast<uint16_t>(fieldIndex(UF::OBJECT_FIELD_ENTRY)), kSignEntry},
                                             {static_cast<uint16_t>(fieldIndex(UF::GAMEOBJECT_DISPLAYID)), 0xD15}}));
    h.pump();
}

/// A type 9 (TEXT) object: the page in data[0], the material in data[2].
std::vector<uint8_t> signTemplate() {
    std::array<uint32_t, 24> data{};
    data[0] = kPage;
    data[2] = kStone;
    return GameHarness::gameObjectTemplateBody(kSignEntry, 9, "Stormwind Notice", data);
}

/// The page CMSG_PAGE_TEXT_QUERY asked for, and of which object.
std::pair<uint32_t, uint64_t> pageAsked(GameHarness& h) {
    auto* query = h.lastSent(Opcode::CMSG_PAGE_TEXT_QUERY);
    REQUIRE(query);
    const uint32_t page = query->readUInt32();
    return {page, query->readUInt64()};
}
}  // namespace

TEST_CASE("a sign clicked before its template came is read when it comes", "[inventory][book][harness]") {
    GameHarness h;
    h.enterWorld();
    signInView(h);
    h.clearRecords();

    h.game().inventory().readGameObject(kSign);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_PAGE_TEXT_QUERY));

    h.receive(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE, signTemplate());
    h.pump();
    CHECK(pageAsked(h) == std::pair<uint32_t, uint64_t>{kPage, kSign});
    CHECK(h.game().inventory().getBookTitle() == "Stormwind Notice");
    CHECK(h.game().inventory().getBookMaterial() == kStone);
}

TEST_CASE("a sign the server offers is read, its page asked for at once", "[inventory][book][harness]") {
    GameHarness h;
    h.enterWorld();
    signInView(h);
    h.receive(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE, signTemplate());
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_GAMEOBJECT_PAGETEXT, Body().u64(kSign).bytes());
    h.pump();
    CHECK(pageAsked(h) == std::pair<uint32_t, uint64_t>{kPage, kSign});
}

// ---------------------------------------------------------------------------
// Equipment sets.
//
// Every guid in this family is packed - a mask byte and then only the non-zero
// bytes - and all of them were read and written as fixed eight-byte values. A
// set guid of one is two bytes on the wire, so the incoming read ate the set id
// and the front of the name, and with more than one set the loop lost its place
// entirely. The outgoing save was worse: a fixed eight bytes where the server
// calls readPackGUID leaves seven behind.
//
// There was also a field on the incoming side that the server does not send: an
// ignore mask read as a uint32 between the icon name and the slots. An ignored
// slot is written as an item guid of literal one, and that is the mask.
//
// The packets are built the way AzerothCore's Player::SendEquipmentSetList
// writes them and read the way HandleEquipmentSetSave reads them.

namespace {

constexpr uint64_t kHelm = 0x40000000000001ull;
constexpr uint64_t kMace = 0x40000000000002ull;

/// One set: packed set guid, index, name, icon, then nineteen packed item
/// guids, one meaning "leave this slot alone". Head, chest ignored, main hand.
void appendSet(Body& body, uint64_t setGuid, uint32_t index, const std::string& name,
               const std::string& icon) {
    body.packedGuid(setGuid).u32(index).cstr(name).cstr(icon);
    for (int slot = 0; slot < 19; ++slot) {
        if (slot == 0) body.packedGuid(kHelm);
        else if (slot == 4) body.packedGuid(1);
        else if (slot == 15) body.packedGuid(kMace);
        else body.packedGuid(0);
    }
}

}  // namespace

TEST_CASE("three equipment sets from the server are three sets in the manager",
          "[inventory][equipment_sets][harness]") {
    GameHarness h;
    h.enterWorld();
    h.clearRecords();

    Body list;
    list.u32(3);
    appendSet(list, 1, 0, "Tanking", "Interface\\Icons\\INV_Shield_06");
    appendSet(list, 0x1234, 1, "Healing", "iconB");
    appendSet(list, 0xFFFFFFFFFFull, 2, "Damage", "iconC");
    h.receive(Opcode::SMSG_EQUIPMENT_SET_LIST, list);
    h.pump();

    const auto& inventory = h.game().inventory();
    const auto& sets = inventory.getEquipmentSets();
    REQUIRE(sets.size() == 3);
    CHECK(sets[0].setGuid == 1);
    CHECK(sets[0].setId == 0);
    CHECK(sets[0].name == "Tanking");
    CHECK(sets[0].iconName == "Interface\\Icons\\INV_Shield_06");
    CHECK(sets[1].name == "Healing");
    CHECK(sets[1].setGuid == 0x1234);
    CHECK(sets[2].name == "Damage");
    CHECK(sets[2].setGuid == 0xFFFFFFFFFFull);
    CHECK(sets[2].setId == 2);

    const auto* items = inventory.getEquipmentSetItems(2);
    REQUIRE(items);
    CHECK((*items)[0] == kHelm);
    CHECK((*items)[15] == kMace);
    // The chest is ignored, not "an item whose guid is one".
    CHECK((*items)[4] == 0);
    CHECK(inventory.getEquipmentSetIgnoreMask(2) == (1u << 4));

    CHECK(h.addonEvents("EQUIPMENT_SETS_CHANGED").size() == 1);
}

TEST_CASE("a new equipment set is saved the way the server reads it",
          "[inventory][equipment_sets][harness]") {
    GameHarness h;
    h.enterWorld();
    h.clearRecords();

    h.game().inventory().saveEquipmentSet("Farming", "iconD");
    auto* save = h.lastSent(Opcode::CMSG_EQUIPMENT_SET_SAVE);
    REQUIRE(save);
    // HandleEquipmentSetSave: readPackGUID, index, name, icon, nineteen packed
    // item guids. A new set has no guid yet, which packs to one mask byte of
    // zero; eight zero bytes would leave the server reading the index out of
    // the padding.
    const std::vector<uint8_t> expectedHead = {
        0x00,                    // packed guid 0
        0x00, 0x00, 0x00, 0x00,  // index 0: the first free one
        'F', 'a', 'r', 'm', 'i', 'n', 'g', 0x00,
        'i', 'c', 'o', 'n', 'D', 0x00,
    };
    const auto& body = save->getData();
    REQUIRE(body.size() == expectedHead.size() + 19);  // nothing worn: 19 empty guids
    CHECK(std::vector<uint8_t>(body.begin(), body.begin() + expectedHead.size()) == expectedHead);
    for (size_t i = expectedHead.size(); i < body.size(); ++i) CHECK(body[i] == 0x00);
}
