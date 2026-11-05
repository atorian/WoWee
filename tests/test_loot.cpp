// Looting: the loot window and the group rolls, and the service against a
// recording gateway and the real event bus. Only wowee_loot is linked.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "loot/services/loot_service.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::loot;

namespace {

template <typename E, typename Changes>
int count(const Changes& changes) {
    return static_cast<int>(std::count_if(changes.begin(), changes.end(),
                                          [](const auto& c) { return std::holds_alternative<E>(c); }));
}

LootItem item(uint8_t slot, uint32_t itemId) {
    LootItem i;
    i.slotIndex = slot;
    i.itemId = itemId;
    i.count = 1;
    return i;
}

LootContents corpse(uint64_t guid, uint32_t gold, std::vector<LootItem> items = {}) {
    LootContents c;
    c.lootGuid = guid;
    c.gold = gold;
    c.items = std::move(items);
    return c;
}

struct RecordingGateway final : ILootGateway, ILootWorld {
    bool world = true;
    bool allowed = true;
    uint64_t usedObject = 0;
    bool opening = false;
    std::map<uint32_t, ItemFacts> items;
    std::map<uint64_t, std::string> names;
    std::vector<std::string> sent;
    std::vector<uint32_t> asked;
    uint64_t purse = 0;

    bool online() const override { return world; }
    bool mayLoot() override { return world && allowed; }
    void sendLoot(uint64_t source) override { sent.push_back("loot " + std::to_string(source)); }
    void sendAutostore(uint8_t slot) override { sent.push_back("take " + std::to_string(slot)); }
    void sendLootMoney() override { sent.push_back("money"); }
    void sendRelease(uint64_t source) override { sent.push_back("release " + std::to_string(source)); }
    void sendMasterGive(uint64_t source, uint8_t slot, uint64_t target) override {
        sent.push_back("give " + std::to_string(source) + ":" + std::to_string(slot) + " to " + std::to_string(target));
    }
    void sendRoll(uint64_t object, uint32_t slot, uint8_t type) override {
        sent.push_back("roll " + std::to_string(object) + ":" + std::to_string(slot) + " " + std::to_string(type));
    }

    std::optional<ItemFacts> item(uint32_t itemId) const override {
        auto it = items.find(itemId);
        if (it == items.end()) return std::nullopt;
        return it->second;
    }
    void requestItem(uint32_t itemId) override { asked.push_back(itemId); }
    std::string playerName(uint64_t guid) const override {
        auto it = names.find(guid);
        return it == names.end() ? std::string() : it->second;
    }
    void creditMoney(uint32_t copper) override { purse += copper; }
    bool isUsedObject(uint64_t guid) const { return usedObject != 0 && guid == usedObject; }
    bool stillOpening(uint64_t guid) const override { return isUsedObject(guid) && opening; }
    bool objectLootArrived(uint64_t guid) override {
        if (!isUsedObject(guid)) return false;
        usedObject = 0;
        return true;
    }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    LootService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subs;

    Fixture() {
        subs.push_back(bus.subscribe<LootOpened>([this](const LootOpened& e) { events.push_back(std::string("opened ") + (e.autoLoot ? "auto" : "manual")); }));
        subs.push_back(bus.subscribe<LootClosed>([this](const auto&) { events.push_back("closed"); }));
        subs.push_back(bus.subscribe<LootSlotCleared>([this](const LootSlotCleared& e) { events.push_back("cleared " + std::to_string(e.slot)); }));
        subs.push_back(bus.subscribe<LootSlotChanged>([this](const LootSlotChanged& e) { events.push_back("changed " + std::to_string(e.slot)); }));
        subs.push_back(bus.subscribe<BindConfirmRequested>([this](const BindConfirmRequested& e) { events.push_back("bind? " + std::to_string(e.slot)); }));
        subs.push_back(bus.subscribe<MoneyAnnounced>([this](const MoneyAnnounced& e) { events.push_back("looted " + std::to_string(e.copper)); }));
        subs.push_back(bus.subscribe<RollStarted>([this](const RollStarted& e) { events.push_back("roll " + std::to_string(e.rollId) + " " + e.itemName); }));
        subs.push_back(bus.subscribe<RollEnded>([this](const RollEnded& e) { events.push_back("roll over " + std::to_string(e.rollId)); }));
        subs.push_back(bus.subscribe<VoteCast>([this](const VoteCast& e) { events.push_back("vote " + e.playerName); }));
        subs.push_back(bus.subscribe<LootItemReceived>([this](const LootItemReceived& e) { events.push_back("received " + std::to_string(e.itemId)); }));
    }

    void open(LootContents c) {
        service.onContents(std::move(c));
        gateway.sent.clear();
        events.clear();
    }
};

} // namespace

TEST_CASE("Taking the coin keeps its slot, so the items keep their buttons", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 50, {item(0, 7)}));
    auto changes = w.moneyCleared();
    REQUIRE(count<LootSlotCleared>(changes) == 1);
    CHECK(std::get<LootSlotCleared>(changes[0]).slot == 1);
    CHECK(w.contents().hasCoinSlot());
    CHECK_FALSE(w.contents().goldLeft());
    CHECK(w.moneyCleared().empty());
}

TEST_CASE("A cleared item slot is named in the interface's numbering", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 0, {item(3, 7)}));
    auto changes = w.slotRemoved(3);
    REQUIRE(changes.size() == 1);
    CHECK(std::get<LootSlotCleared>(changes[0]).slot == 4);
    CHECK(w.contents().items[0].looted);
    CHECK(w.slotRemoved(5).empty());
}

TEST_CASE("Money is announced once per source", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 50));
    CHECK(count<MoneyAnnounced>(w.moneyNotified(50)) == 1);
    w.opened(corpse(1, 50));
    CHECK(count<MoneyAnnounced>(w.moneyNotified(50)) == 0);
}

TEST_CASE("A share of a group's money leaves this corpse's coin alone", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 90));
    w.moneyNotified(30);
    CHECK(w.contents().goldLeft());
    w.moneyNotified(90);
    CHECK_FALSE(w.contents().goldLeft());
}

TEST_CASE("Money a server never confirms is announced after a moment", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 70));
    w.moneyRequested();
    CHECK(w.timePassed(0.3f).empty());
    auto changes = w.timePassed(0.2f);
    REQUIRE(changes.size() == 1);
    CHECK(std::get<MoneyAnnounced>(changes[0]).copper == 70);
    CHECK(w.timePassed(1.0f).empty());
}

TEST_CASE("A source announced a moment ago arms no second announcement", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 70));
    w.moneyRequested();
    w.timePassed(0.5f);
    w.released();
    w.opened(corpse(1, 70));
    w.moneyRequested();
    CHECK(w.timePassed(0.5f).empty());
}

TEST_CASE("The announcement cooldown runs out", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 70));
    w.moneyRequested();
    w.timePassed(0.5f);
    w.released();          // forgets the source, so only the cooldown can stop it
    w.timePassed(2.0f);
    w.opened(corpse(1, 70));
    w.moneyRequested();
    CHECK(count<MoneyAnnounced>(w.timePassed(0.5f)) == 1);
}

TEST_CASE("A binding item asks first, counting the coin as a slot", "[loot]") {
    LootWindow w;
    w.opened(corpse(1, 20, {item(0, 7), item(1, 8)}));
    auto changes = w.holdForBind(1);
    REQUIRE(changes.size() == 1);
    CHECK(std::get<BindConfirmRequested>(changes[0]).slot == 3);
    CHECK(w.takePendingBind() == uint8_t{1});
    CHECK_FALSE(w.takePendingBind());
}

TEST_CASE("A late item template redraws the rows that name it", "[loot]") {
    LootWindow w;
    CHECK(w.itemInfoArrived(7).empty());
    w.opened(corpse(1, 20, {item(0, 9), item(1, 7)}));
    auto changes = w.itemInfoArrived(7);
    REQUIRE(changes.size() == 1);
    CHECK(std::get<LootSlotChanged>(changes[0]).slot == 3);
}

TEST_CASE("A restated roll keeps its id; a new one gets the next", "[loot]") {
    LootRolls rolls;
    LootRoll a;
    a.objectGuid = 1;
    a.slot = 0;
    LootRoll b = a;
    b.slot = 1;
    CHECK(std::get<RollStarted>(rolls.started(a)[0]).rollId == 1);
    CHECK(std::get<RollStarted>(rolls.started(b)[0]).rollId == 2);
    a.voteMask = 0x01;
    CHECK(std::get<RollStarted>(rolls.started(a)[0]).rollId == 1);
    CHECK(rolls.find(1)->voteMask == 0x01);
    CHECK(std::get<RollEnded>(rolls.ended(1, 1)[0]).rollId == 2);
    CHECK_FALSE(rolls.find(2));
    CHECK(rolls.ended(1, 1).empty());
}

TEST_CASE("Opening loot asks for its money and its item names", "[loot]") {
    Fixture f;
    f.service.onContents(corpse(1, 50, {item(0, 7)}));
    CHECK(f.events == std::vector<std::string>{"opened manual"});
    CHECK(f.gateway.sent == std::vector<std::string>{"money"});
    CHECK(f.gateway.asked == std::vector<uint32_t>{7});
    CHECK(f.service.isLootWindowOpen());
}

TEST_CASE("Autoloot stores every item, once per source", "[loot]") {
    Fixture f;
    f.service.setAutoLoot(true);
    f.service.onContents(corpse(1, 0, {item(0, 7), item(2, 8)}));
    CHECK(f.gateway.sent == std::vector<std::string>{"take 0", "take 2"});
    CHECK(f.events == std::vector<std::string>{"opened auto"});
    f.gateway.sent.clear();
    f.service.onContents(corpse(1, 0, {item(2, 8)}));
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("A used object's items are stored with autoloot off", "[loot]") {
    Fixture f;
    f.gateway.usedObject = 5;
    f.service.onContents(corpse(5, 0, {item(0, 7)}));
    CHECK(f.gateway.sent == std::vector<std::string>{"take 0"});
    CHECK(f.gateway.usedObject == 0);
}

TEST_CASE("An empty answer from a node still being gathered is not its loot", "[loot]") {
    Fixture f;
    f.gateway.usedObject = 5;
    f.gateway.opening = true;
    f.service.onContents(corpse(5, 0));
    CHECK(f.events.empty());
    CHECK_FALSE(f.service.isLootWindowOpen());
}

TEST_CASE("Loot with nothing in it closes at once", "[loot]") {
    Fixture f;
    f.service.onContents(corpse(1, 0));
    CHECK(f.events == std::vector<std::string>{"opened manual", "closed"});
    CHECK(f.gateway.sent == std::vector<std::string>{"release 1"});
}

TEST_CASE("The last item taken closes the window", "[loot]") {
    Fixture f;
    f.open(corpse(1, 0, {item(0, 7), item(1, 8)}));
    f.service.onSlotRemoved(0);
    CHECK(f.service.isLootWindowOpen());
    f.service.onSlotRemoved(1);
    CHECK(f.events == std::vector<std::string>{"cleared 1", "cleared 2", "closed"});
    CHECK(f.gateway.sent == std::vector<std::string>{"release 1"});
}

TEST_CASE("The money notify fills the purse and empties a money-only corpse", "[loot]") {
    Fixture f;
    f.open(corpse(1, 40));
    f.service.onMoneyNotify(40);
    CHECK(f.gateway.purse == 40);
    CHECK(f.events == std::vector<std::string>{"looted 40", "cleared 1", "closed"});
}

TEST_CASE("Closing asks the server to release only an open window", "[loot]") {
    Fixture f;
    f.service.closeLoot();
    CHECK(f.gateway.sent.empty());
    f.open(corpse(1, 0, {item(0, 7)}));
    f.service.closeLoot();
    f.service.closeLoot();
    CHECK(f.gateway.sent == std::vector<std::string>{"release 1"});
    CHECK(f.events == std::vector<std::string>{"closed"});
}

TEST_CASE("A bind-on-pickup item waits for the player's yes", "[loot]") {
    Fixture f;
    f.gateway.items[7] = {"Ashkandi", 4, true};
    f.gateway.items[8] = {"Linen", 1, false};
    f.open(corpse(1, 0, {item(0, 7), item(1, 8)}));
    f.service.lootItem(1);
    f.service.lootItem(0);
    CHECK(f.gateway.sent == std::vector<std::string>{"take 1"});
    CHECK(f.events == std::vector<std::string>{"bind? 1"});
    f.service.confirmPendingLoot();
    f.service.confirmPendingLoot();
    CHECK(f.gateway.sent == std::vector<std::string>{"take 1", "take 0"});
}

TEST_CASE("Master loot gives from the open loot", "[loot]") {
    Fixture f;
    f.open(corpse(9, 0, {item(0, 7)}));
    f.service.onMasterCandidates({100, 200});
    f.service.lootMasterGive(0, 200);
    CHECK(f.gateway.sent == std::vector<std::string>{"give 9:0 to 200"});
    CHECK(f.service.getMasterLootCandidates().size() == 2);
}

TEST_CASE("Only a corpse that held items is remembered as worth a look", "[loot]") {
    Fixture f;
    f.open(corpse(1, 0, {item(0, 7)}));
    f.open(corpse(2, 30));
    CHECK(f.service.corpseHeldItems(1));
    CHECK_FALSE(f.service.corpseHeldItems(2));
    CHECK_FALSE(f.service.corpseHeldItems(3));
}

TEST_CASE("An item notify names the item in that slot", "[loot]") {
    Fixture f;
    f.open(corpse(1, 0, {item(4, 7)}));
    f.service.onItemNotify(4);
    f.service.onItemNotify(5);
    CHECK(f.events == std::vector<std::string>{"received 7"});
}

TEST_CASE("A roll names its item, and answering it closes only that roll", "[loot]") {
    Fixture f;
    f.gateway.items[7] = {"Ashkandi", 4, true};
    f.service.onRollStarted(1, 0, 7, 60000, 0x07);
    f.service.onRollStarted(1, 1, 8, 60000, 0x07);
    CHECK(f.events == std::vector<std::string>{"roll 1 Ashkandi", "roll 2 Item #8"});
    CHECK(f.service.getLootRoll(2)->itemQuality == 1);
    f.service.sendLootRoll(1, 1, 2);
    CHECK(f.gateway.sent == std::vector<std::string>{"roll 1:1 2"});
    CHECK(f.service.getLootRoll(1));
    CHECK_FALSE(f.service.getLootRoll(2));
}

TEST_CASE("A roll ends when it is won or everyone passes", "[loot]") {
    Fixture f;
    f.service.onRollStarted(1, 0, 7, 60000, 0x07);
    f.service.onRollStarted(1, 1, 8, 60000, 0x07);
    f.events.clear();
    f.service.onRollWon(1, 0, 7, 0, 50, 99, 1);
    f.service.onAllPassed(1, 1, 8);
    CHECK(f.events == std::vector<std::string>{"roll over 1", "roll over 2"});
}

TEST_CASE("A vote from someone not yet named says Player", "[loot]") {
    Fixture f;
    f.gateway.names[50] = "Jaina";
    f.service.onVote(50, 12, 1);
    f.service.onVote(51, 3, 0);
    CHECK(f.events == std::vector<std::string>{"vote Jaina", "vote Player"});
}

TEST_CASE("Nothing is sent while offline", "[loot]") {
    Fixture f;
    f.open(corpse(1, 0, {item(0, 7)}));
    f.gateway.world = false;
    f.service.lootTarget(1);
    f.service.lootItem(0);
    f.service.lootMoney();
    f.service.sendLootRoll(1, 0, 1);
    f.service.closeLoot();
    CHECK(f.gateway.sent.empty());
    CHECK_FALSE(f.service.isLootWindowOpen());
}

TEST_CASE("Closing a window that is not open says nothing", "[loot]") {
    LootWindow w;
    CHECK(w.closed().empty());
    w.opened(corpse(1, 0, {item(0, 7)}));
    CHECK(count<LootClosed>(w.closed()) == 1);
    CHECK(w.closed().empty());
}

TEST_CASE("Nothing is asked of the server while looting is not allowed", "[loot]") {
    Fixture f;
    f.open(corpse(1, 0, {item(0, 7)}));
    f.gateway.allowed = false;
    f.service.lootTarget(2);
    f.service.lootItem(0);
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("The next character has none of the last one's loot or rolls, and keeps autoloot", "[loot]") {
    Fixture f;
    f.service.setAutoLoot(true);
    f.open(corpse(1, 30, {item(0, 7)}));
    f.service.onMasterCandidates({100});
    f.service.onRollStarted(1, 0, 7, 60000, 0x07);

    f.service.beginCharacter();

    CHECK_FALSE(f.service.isLootWindowOpen());
    CHECK(f.service.getCurrentLoot().items.empty());
    CHECK(f.service.getMasterLootCandidates().empty());
    CHECK_FALSE(f.service.getLootRoll(1));
    CHECK_FALSE(f.service.corpseHeldItems(1));
    CHECK(f.service.isAutoLoot());
    // The money fallback armed for the last character's corpse does not fire.
    f.events.clear();
    f.service.timePassed(1.0f);
    CHECK(f.events.empty());
}
