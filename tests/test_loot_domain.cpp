// Looting, end to end: the realm's loot and roll packets, what the loot frame,
// the roll frames and the chat are told, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "loot/domain/events.hpp"
#include "loot/domain/records.hpp"
#include "loot/services/loot.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kCorpse = 0xF130000001000001ull;
constexpr uint64_t kOtherCorpse = 0xF130000001000002ull;
constexpr uint32_t kLinen = 2589;
constexpr uint32_t kWool = 2592;

/// SMSG_LOOT_RESPONSE for a corpse holding money and nothing else, as 3.3.5a
/// writes it: the loot guid, the loot type, the money, no items and no quest
/// items.
std::vector<uint8_t> moneyLoot(uint64_t lootGuid, uint32_t copper) {
    return Body().u64(lootGuid).u8(1 /* corpse */).u32(copper).u8(0).u8(0);
}

/// SMSG_LOOT_RESPONSE for a corpse holding these items, one of each, in
/// server slots 0, 1, ... and no money.
std::vector<uint8_t> itemLoot(uint64_t lootGuid, const std::vector<uint32_t>& items) {
    Body b;
    b.u64(lootGuid).u8(1).u32(0).u8(static_cast<uint8_t>(items.size()));
    for (size_t i = 0; i < items.size(); ++i) {
        b.u8(static_cast<uint8_t>(i)).u32(items[i]).u32(1);   // slot, item, count
        b.u32(0).u32(0).u32(0).u8(0);                         // display, suffix, property, slot type
    }
    b.u8(0);                                                  // no quest items
    return b.bytes();
}

/// SMSG_LOOT_START_ROLL as 3.3.5a writes it, on a slot of kCorpse.
std::vector<uint8_t> rollStarts(uint32_t slot, uint32_t itemId, uint32_t countdownMs) {
    return Body().u64(kCorpse).u32(0 /* map */).u32(slot).u32(itemId)
        .u32(0).u32(0).u32(1 /* in stack */).u32(countdownMs).u8(0x07).bytes();
}

/// Every MoneyAnnounced the game announces, while it lives.
struct MoneyAnnounced {
    explicit MoneyAnnounced(GameHarness& h)
        : sub(h.events().subscribe<wowee::loot::MoneyAnnounced>(
              [this](const wowee::loot::MoneyAnnounced& e) { seen.push_back(e); })) {}
    std::vector<wowee::loot::MoneyAnnounced> seen;
    wowee::core::EventBus::Subscription sub;
};

size_t linesStarting(GameHarness& h, const std::string& prefix) {
    const auto& history = h.game().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(), [&](const auto& line) {
        return line.message.rfind(prefix, 0) == 0;
    }));
}

void openCorpse(GameHarness& h, const std::vector<uint32_t>& items) {
    h.receive(Opcode::SMSG_LOOT_RESPONSE, itemLoot(kCorpse, items));
    h.pump();
}

}  // namespace

TEST_CASE("money the server confirms is taken, counted and announced once", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);
    const uint64_t before = h.game().inventory().moneyCopper();

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 1234));
    h.pump();
    CHECK(h.addonEvents("LOOT_OPENED").size() == 1);
    CHECK(h.countSent(Opcode::CMSG_LOOT_MONEY) == 1);

    h.receive(Opcode::SMSG_LOOT_MONEY_NOTIFY, Body().u32(1234).u8(1));
    h.pump();
    REQUIRE(money.seen.size() == 1);
    CHECK(money.seen[0].copper == 1234);
    CHECK(money.seen[0].lootGuid == kCorpse);
    CHECK(h.game().inventory().moneyCopper() == before + 1234);
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);

    // The fallback for a server that never confirms must not say it again.
    h.advance(1.0f);
    CHECK(money.seen.size() == 1);
    CHECK(linesStarting(h, "Looted: ") == 1);
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
    CHECK(linesStarting(h, "Looted: ") == 1);
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

    // The cooldown that stops one loot being announced twice runs out.
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
    auto opened = h.events().subscribe<wowee::loot::LootOpened>(
        [&](const wowee::loot::LootOpened&) { seen.push_back(true); });
    auto closed = h.events().subscribe<wowee::loot::LootClosed>(
        [&](const wowee::loot::LootClosed&) { seen.push_back(false); });

    openCorpse(h, {kLinen});
    REQUIRE(seen == std::vector<bool>{true});
    CHECK(h.loot().isLootWindowOpen());

    h.loot().closeLoot();
    CHECK(seen == std::vector<bool>{true, false});
    auto* release = h.lastSent(Opcode::CMSG_LOOT_RELEASE);
    REQUIRE(release);
    CHECK(release->readUInt64() == kCorpse);
}

TEST_CASE("each item taken is cleared from the window, and the last one closes it", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    openCorpse(h, {kLinen, kWool});
    const auto opened = h.addonEvents("LOOT_OPENED");
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].args == std::vector<std::string>{"0"});   // not taken automatically
    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUTOSTORE_LOOT_ITEM));

    h.loot().lootItem(1);
    auto* take = h.lastSent(Opcode::CMSG_AUTOSTORE_LOOT_ITEM);
    REQUIRE(take);
    CHECK(take->readUInt8() == 1);

    h.receive(Opcode::SMSG_LOOT_REMOVED, Body().u8(1).bytes());
    h.pump();
    CHECK(h.loot().isLootWindowOpen());
    CHECK_FALSE(h.lastSent(Opcode::CMSG_LOOT_RELEASE));

    h.receive(Opcode::SMSG_LOOT_REMOVED, Body().u8(0).bytes());
    h.pump();
    // The interface counts its slots from one.
    const auto cleared = h.addonEvents("LOOT_SLOT_CLEARED");
    REQUIRE(cleared.size() == 2);
    CHECK(cleared[0].args == std::vector<std::string>{"2"});
    CHECK(cleared[1].args == std::vector<std::string>{"1"});
    CHECK_FALSE(h.loot().isLootWindowOpen());
    CHECK(h.lastSent(Opcode::CMSG_LOOT_RELEASE));
    CHECK(h.addonEvents("LOOT_CLOSED").size() == 1);
}

TEST_CASE("with autoloot on, every item is taken as the window opens", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    h.loot().setAutoLoot(true);

    openCorpse(h, {kLinen, kWool});

    CHECK(h.countSent(Opcode::CMSG_AUTOSTORE_LOOT_ITEM) == 2);
    const auto opened = h.addonEvents("LOOT_OPENED");
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].args == std::vector<std::string>{"1"});
}

TEST_CASE("a loot response that does not parse leaves the open window as it was", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    openCorpse(h, {kLinen});

    // Another corpse, declined: the realm's loot type with nothing behind it.
    h.receive(Opcode::SMSG_LOOT_RESPONSE, Body().u64(kOtherCorpse).u8(0).bytes());
    h.pump();

    REQUIRE(h.loot().isLootWindowOpen());
    const auto& loot = h.loot().getCurrentLoot();
    CHECK(loot.lootGuid == kCorpse);
    REQUIRE(loot.items.size() == 1);
    CHECK(loot.items[0].itemId == kLinen);
}

TEST_CASE("the master loot menu is told its candidates, and a list cut short keeps the last one",
          "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    openCorpse(h, {kLinen});

    h.receive(Opcode::SMSG_LOOT_MASTER_LIST, Body().u8(2).u64(0x21).u64(0x22).bytes());
    h.pump();
    CHECK(h.loot().getMasterLootCandidates() == std::vector<uint64_t>{0x21, 0x22});
    CHECK(h.addonEvents("OPEN_MASTER_LOOT_LIST").size() == 1);
    CHECK(h.firedAt("OPEN_MASTER_LOOT_LIST") < h.firedAt("UPDATE_MASTER_LOOT_LIST"));

    h.loot().lootMasterGive(0, 0x22);
    auto* give = h.lastSent(Opcode::CMSG_LOOT_MASTER_GIVE);
    REQUIRE(give);
    CHECK(give->readUInt64() == kCorpse);
    CHECK(give->readUInt8() == 0);
    CHECK(give->readUInt64() == 0x22);

    h.receive(Opcode::SMSG_LOOT_MASTER_LIST, Body().bytes());
    h.pump();
    CHECK(h.loot().getMasterLootCandidates() == std::vector<uint64_t>{0x21, 0x22});
    CHECK(h.addonEvents("OPEN_MASTER_LOOT_LIST").size() == 1);
}

TEST_CASE("an item the player is given from loot is said, unless loot spam is off", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    openCorpse(h, {kLinen, kWool});

    h.receive(Opcode::SMSG_LOOT_ITEM_NOTIFY, Body().u64(kCorpse).u8(0).bytes());
    h.pump();
    CHECK(linesStarting(h, "You receive loot: ") == 1);

    h.cvars().set("showLootSpam", "0");
    h.receive(Opcode::SMSG_LOOT_ITEM_NOTIFY, Body().u64(kCorpse).u8(1).bytes());
    h.pump();
    CHECK(linesStarting(h, "You receive loot: ") == 1);
}

TEST_CASE("a group roll opens a frame by its own id, and answering it closes that frame",
          "[loot][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LOOT_START_ROLL, rollStarts(0, kLinen, 60000));
    h.pump();
    const auto started = h.addonEvents("START_LOOT_ROLL");
    REQUIRE(started.size() == 1);
    CHECK(started[0].args == std::vector<std::string>{"1", "60000"});
    CHECK(linesStarting(h, "Loot roll started for ") == 1);
    const auto* roll = h.loot().getLootRoll(1);
    REQUIRE(roll);
    CHECK(roll->itemId == kLinen);
    CHECK(roll->voteMask == 0x07);

    h.loot().sendLootRoll(roll->objectGuid, roll->slot, 2);
    auto* vote = h.lastSent(Opcode::CMSG_LOOT_ROLL);
    REQUIRE(vote);
    CHECK(vote->readUInt64() == kCorpse);
    CHECK(vote->readUInt32() == 0);
    CHECK(vote->readUInt8() == 2);
    h.pump();
    const auto cancelled = h.addonEvents("CANCEL_LOOT_ROLL");
    REQUIRE(cancelled.size() == 1);
    CHECK(cancelled[0].args == std::vector<std::string>{"1"});
    CHECK_FALSE(h.loot().getLootRoll(1));
}

TEST_CASE("a roll won while the player had not answered closes that roll's frame and no other",
          "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_LOOT_START_ROLL, rollStarts(0, kLinen, 60000));
    h.receive(Opcode::SMSG_LOOT_START_ROLL, rollStarts(1, kWool, 60000));
    h.pump();
    REQUIRE(h.addonEvents("START_LOOT_ROLL").size() == 2);

    // Slot 1 won by someone the client has no name for, on need with 87.
    h.receive(Opcode::SMSG_LOOT_ROLL_WON,
              Body().u64(kCorpse).u32(1).u32(kWool).u32(0).u32(0).u64(0x99).u8(87).u8(1).bytes());
    h.pump();

    const auto cancelled = h.addonEvents("CANCEL_LOOT_ROLL");
    REQUIRE(cancelled.size() == 1);
    CHECK(cancelled[0].args == std::vector<std::string>{"2"});
    CHECK(h.loot().getLootRoll(1));
    CHECK_FALSE(h.loot().getLootRoll(2));
    CHECK(linesStarting(h, "Player won ") == 1);
}

TEST_CASE("the next character has none of the last one's loot or rolls", "[loot][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    openCorpse(h, {kLinen});
    h.receive(Opcode::SMSG_LOOT_START_ROLL, rollStarts(0, kWool, 60000));
    h.pump();
    REQUIRE(h.loot().isLootWindowOpen());
    REQUIRE(h.loot().getLootRoll(1));

    h.relogAs(bob);

    CHECK_FALSE(h.loot().isLootWindowOpen());
    CHECK(h.loot().getCurrentLoot().items.empty());
    CHECK_FALSE(h.loot().getLootRoll(1));
    CHECK_FALSE(h.loot().corpseHeldItems(kCorpse));
}
