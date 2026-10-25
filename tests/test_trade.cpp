// A trade: its domain model, and the service against a recording gateway and
// the real event bus. Only wowee_trade is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "trade/services/trade_service.hpp"

#include <map>
#include <string>
#include <vector>

using namespace wowee::trade;

namespace {

OfferedSlot offered(uint8_t index, uint32_t itemId, uint32_t count = 1) {
    OfferedSlot s;
    s.index = index;
    s.itemId = itemId;
    s.stackCount = count;
    return s;
}

struct RecordingGateway final : ITradeGateway, ITradeWorld {
    bool world = true, blocked = false;
    std::map<uint64_t, std::string> names;
    std::vector<std::string> sent;
    std::vector<uint32_t> asked;

    bool online() const override { return world; }
    bool mayAskToTrade() override { return world; }
    void sendInitiate(uint64_t target) override { sent.push_back("initiate " + std::to_string(target)); }
    void sendBegin() override { sent.push_back("begin"); }
    void sendCancel() override { sent.push_back("cancel"); }
    void sendAccept() override { sent.push_back("accept"); }
    void sendUnaccept() override { sent.push_back("unaccept"); }
    void sendSetItem(uint8_t slot, uint8_t, uint8_t) override { sent.push_back("item " + std::to_string(slot)); }
    void sendClearItem(uint8_t slot) override { sent.push_back("clear " + std::to_string(slot)); }
    void sendSetGold(uint32_t copper) override { sent.push_back("gold " + std::to_string(copper)); }

    std::string playerName(uint64_t guid) const override {
        auto it = names.find(guid);
        return it == names.end() ? std::string() : it->second;
    }
    bool tradesBlocked() const override { return blocked; }
    void requestItem(uint32_t entry) override { asked.push_back(entry); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    TradeService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<TradeRequested>([this](const TradeRequested& e) { events.push_back("requested " + e.peerName); }));
        subscriptions.push_back(bus.subscribe<TradeRequestBlocked>([this](const TradeRequestBlocked& e) { events.push_back("blocked " + e.peerName); }));
        subscriptions.push_back(bus.subscribe<TradeProposed>([this](const auto&) { events.push_back("proposed"); }));
        subscriptions.push_back(bus.subscribe<NoTradeTarget>([this](const auto&) { events.push_back("no target"); }));
        subscriptions.push_back(bus.subscribe<AcceptStateChanged>([this](const AcceptStateChanged& e) {
            events.push_back(std::string("accept ") + (e.self ? "1" : "0") + (e.partner ? "1" : "0"));
        }));
        subscriptions.push_back(bus.subscribe<TradeEnded>([this](const auto&) { events.push_back("ended"); }));
        subscriptions.push_back(bus.subscribe<TradeRefused>([this](const auto&) { events.push_back("refused"); }));
        subscriptions.push_back(bus.subscribe<OfferedMoneyChanged>([this](const OfferedMoneyChanged& e) {
            events.push_back(e.own ? "my money" : "their money");
        }));
    }

    /// A trade with player 5 is open.
    void trading() {
        service.onStatus(StatusCode::BeginTrade, 5);
        service.onStatus(StatusCode::OpenWindow, 0);
        gateway.sent.clear();
        events.clear();
    }
};

} // namespace

TEST_CASE("A completed trade keeps what was traded readable", "[trade]") {
    Trade t;
    t.requested(5, "Thrall");
    t.opened();
    t.offerReceived(false, 0, {offered(0, 42)});
    t.ended(Ending::Completed);
    CHECK_FALSE(t.isOpen());
    CHECK(t.peerSlots()[0].itemId == 42);
    CHECK(t.peerGuid() == 5);
}

TEST_CASE("A cancelled trade forgets everything", "[trade]") {
    Trade t;
    t.requested(5, "Thrall");
    t.opened();
    t.offerReceived(true, 300, {offered(1, 42)});
    t.ended(Ending::Cancelled);
    CHECK(t.mySlots()[1].itemId == 0);
    CHECK(t.myGold() == 0);
    CHECK(t.peerGuid() == 0);
}

TEST_CASE("Being stunned does not end a trade; being too far does", "[trade]") {
    Trade t;
    t.requested(5, "Thrall");
    t.opened();
    t.refused(Refusal::YouStunned);
    CHECK(t.isOpen());
    t.refused(Refusal::TooFar);
    CHECK_FALSE(t.isOpen());
}

TEST_CASE("An offer fills its own side, and money is announced when it moves", "[trade]") {
    Trade t;
    t.offerReceived(true, 0, {offered(2, 7, 3), offered(kSlotCount, 9)});
    CHECK(t.mySlots()[2].stackCount == 3);
    CHECK(t.peerSlots()[2].itemId == 0);
    // The eighth slot is no slot: nothing lands past the seventh.
    CHECK(t.peerSlots()[0].itemId == 0);
    auto changes = t.offerReceived(false, 500, {});
    CHECK(changes.size() == 2);
    CHECK(t.peerGold() == 500);
    CHECK(t.offerReceived(false, 500, {}).size() == 1);
}

TEST_CASE("A request names the player asking, or Unknown", "[trade]") {
    Fixture f;
    f.gateway.names[5] = "Thrall";
    f.service.onStatus(StatusCode::BeginTrade, 5);
    f.service.onStatus(StatusCode::BeginTrade, 6);
    CHECK(f.events == std::vector<std::string>{"requested Thrall", "requested Unknown"});
}

TEST_CASE("A blocked request is turned down before any window", "[trade]") {
    Fixture f;
    f.gateway.blocked = true;
    f.gateway.names[5] = "Thrall";
    f.service.onStatus(StatusCode::BeginTrade, 5);
    CHECK(f.events == std::vector<std::string>{"blocked Thrall"});
    CHECK(f.gateway.sent == std::vector<std::string>{"cancel"});
    f.service.acceptTradeRequest();
    CHECK(f.gateway.sent.size() == 1);
}

TEST_CASE("Only a pending request can be answered", "[trade]") {
    Fixture f;
    f.service.acceptTradeRequest();
    CHECK(f.gateway.sent.empty());
    f.service.onStatus(StatusCode::BeginTrade, 5);
    f.service.acceptTradeRequest();
    CHECK(f.gateway.sent == std::vector<std::string>{"begin"});
}

TEST_CASE("Whoever accepts second can still accept", "[trade]") {
    Fixture f;
    f.trading();
    f.service.onStatus(StatusCode::Accepted, 0);
    f.service.acceptTrade();
    CHECK(f.gateway.sent == std::vector<std::string>{"accept"});
    CHECK(f.events == std::vector<std::string>{"accept 01", "accept 11"});
}

TEST_CASE("Only one's own acceptance can be taken back", "[trade]") {
    Fixture f;
    f.trading();
    f.service.onStatus(StatusCode::Accepted, 0);
    f.service.unacceptTrade();
    CHECK(f.gateway.sent.empty());
    f.service.acceptTrade();
    f.service.unacceptTrade();
    CHECK(f.gateway.sent == std::vector<std::string>{"accept", "unaccept"});
    CHECK(f.events.back() == "accept 01");
}

TEST_CASE("Back to trade clears both acceptances", "[trade]") {
    Fixture f;
    f.trading();
    f.service.acceptTrade();
    f.service.onStatus(StatusCode::BackToTrade, 0);
    CHECK(f.events.back() == "accept 00");
    f.service.unacceptTrade();
    CHECK(f.gateway.sent == std::vector<std::string>{"accept"});
}

TEST_CASE("Items and gold change only while nobody has accepted", "[trade]") {
    Fixture f;
    f.trading();
    f.service.setTradeItem(0, 255, 23);
    f.service.setTradeGold(100);
    f.service.onStatus(StatusCode::Accepted, 0);
    f.service.clearTradeItem(0);
    f.service.setTradeGold(200);
    CHECK(f.gateway.sent == std::vector<std::string>{"item 0", "gold 100"});
}

TEST_CASE("Cancelling ends the trade here and tells the server", "[trade]") {
    Fixture f;
    f.service.cancelTrade();
    CHECK(f.gateway.sent.empty());
    f.trading();
    f.service.cancelTrade();
    CHECK(f.gateway.sent == std::vector<std::string>{"cancel"});
    CHECK_FALSE(f.service.isTradeOpen());
}

TEST_CASE("Proposing a trade needs someone to trade with", "[trade]") {
    Fixture f;
    f.service.initiateTrade(0);
    CHECK(f.events == std::vector<std::string>{"no target"});
    f.service.initiateTrade(5);
    CHECK(f.gateway.sent == std::vector<std::string>{"initiate 5"});
    CHECK(f.events.back() == "proposed");
}

TEST_CASE("Nothing is sent while offline", "[trade]") {
    Fixture f;
    f.trading();
    f.gateway.world = false;
    f.service.initiateTrade(5);
    f.service.acceptTrade();
    f.service.setTradeGold(1);
    f.service.cancelTrade();
    CHECK(f.gateway.sent.empty());
    CHECK(f.events.empty());
    CHECK_FALSE(f.service.isTradeOpen());
}

TEST_CASE("An offer's items are looked up, and each side's money says whose", "[trade]") {
    Fixture f;
    f.trading();
    f.service.onOffer(true, 50, {offered(0, 11), offered(1, 0)});
    f.service.onOffer(false, 70, {offered(0, 22)});
    CHECK(f.gateway.asked == std::vector<uint32_t>{11, 22});
    CHECK(f.events == std::vector<std::string>{"my money", "their money"});
    CHECK(f.service.getMyTradeGold() == 50);
}

TEST_CASE("A trade ends on distance, not on being stunned", "[trade]") {
    Fixture f;
    f.trading();
    f.service.onStatus(StatusCode::YouStunned, 0);
    CHECK(f.service.isTradeOpen());
    f.service.onStatus(StatusCode::TooFar, 0);
    CHECK_FALSE(f.service.isTradeOpen());
    CHECK(f.events == std::vector<std::string>{"refused", "refused"});
}

TEST_CASE("The next character has none of the last one's trade", "[trade]") {
    Fixture f;
    f.trading();
    f.service.onOffer(false, 500, {OfferedSlot{0, 11, 2, 1}});

    f.service.beginCharacter();

    CHECK_FALSE(f.service.isTradeOpen());
    CHECK(f.service.getTradePeerGuid() == 0);
    CHECK(f.service.getPeerTradeGold() == 0);
    CHECK(f.service.getPeerTradeSlots()[0].itemId == 0);
    // Nothing to accept, so nothing goes out.
    f.service.acceptTrade();
    CHECK(f.gateway.sent.empty());
}
