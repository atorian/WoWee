// The auction house: its domain model, and the service against a recording
// gateway and the real event bus. Only wowee_auction is linked.
#include <catch_amalgamated.hpp>

#include "auction/services/auction_service.hpp"
#include "foundation/event_bus.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::auction;

namespace {

ListResult listOf(std::vector<uint32_t> items, uint32_t searchDelayMs = 0) {
    ListResult r;
    for (uint32_t item : items) {
        Entry e;
        e.auctionId = item * 10;
        e.itemEntry = item;
        r.auctions.push_back(e);
    }
    r.searchDelay = searchDelayMs;
    return r;
}

std::vector<AuctionList> changedLists(const std::vector<AuctionChange>& changes) {
    std::vector<AuctionList> out;
    for (const auto& c : changes)
        if (auto* l = std::get_if<ListChanged>(&c)) out.push_back(l->list);
    return out;
}

struct RecordingGateway final : IAuctionGateway, IAuctionWorld {
    bool inWorld = true;
    std::vector<std::string> sent;
    std::map<int, BackpackItem> backpack;
    std::vector<uint32_t> prepared;

    bool canSend() const override { return inWorld; }
    void sendSearch(uint64_t a, const SearchQuery& q) override {
        sent.push_back("search " + std::to_string(a) + " '" + q.name + "' sort " + std::to_string(q.sort.size()));
    }
    void sendSell(uint64_t, uint64_t item, uint32_t stack, uint32_t, uint32_t, uint32_t) override {
        sent.push_back("sell " + std::to_string(item) + " x" + std::to_string(stack));
    }
    void sendBid(uint64_t, uint32_t id, uint32_t amount) override {
        sent.push_back("bid " + std::to_string(id) + " " + std::to_string(amount));
    }
    void sendCancel(uint64_t, uint32_t id) override { sent.push_back("cancel " + std::to_string(id)); }
    void sendListOwned(uint64_t, uint32_t) override { sent.push_back("owned"); }
    void sendListBids(uint64_t, uint32_t) override { sent.push_back("bids"); }
    std::optional<BackpackItem> backpackItem(int index) const override {
        auto it = backpack.find(index);
        return it == backpack.end() ? std::nullopt : std::optional<BackpackItem>(it->second);
    }
    void prepareToShow(const Entry& e) override { prepared.push_back(e.itemEntry); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    AuctionService service{gateway, gateway, bus};

    void openAt(uint64_t auctioneer) {
        service.onHello(auctioneer);
        gateway.sent.clear();
    }
};

} // namespace

TEST_CASE("A list goes where it was asked for, or where its opcode says", "[auction]") {
    AuctionHouse house;
    house.opened(5);
    house.listRequested(AuctionList::Owned);
    CHECK(changedLists(house.listReceived(listOf({1}), std::nullopt)) == std::vector{AuctionList::Owned});
    house.searchSent(SearchQuery{});
    CHECK(changedLists(house.listReceived(listOf({2, 3}), std::nullopt)) == std::vector{AuctionList::Browse});
    house.listReceived(listOf({4}), AuctionList::Bids);
    CHECK(house.list(AuctionList::Browse).auctions.size() == 2);
    CHECK(house.list(AuctionList::Bids).auctions.size() == 1);
    // The opcode's list is remembered for the plain list result after it.
    CHECK(changedLists(house.listReceived(listOf({6}), std::nullopt)) == std::vector{AuctionList::Bids});
}

TEST_CASE("The search delay is the server's when it sends one", "[auction]") {
    AuctionHouse house;
    house.searchSent(SearchQuery{});
    CHECK(house.searchDelay() == Catch::Approx(5.0f));
    house.listReceived(listOf({}), std::nullopt);     // Classic and TBC send no delay
    CHECK(house.searchDelay() == Catch::Approx(5.0f));
    house.listReceived(listOf({}, 300), std::nullopt);
    CHECK(house.searchDelay() == Catch::Approx(0.3f));
    house.elapse(1.0f);
    CHECK(house.searchDelay() == 0.0f);
}

TEST_CASE("Opening starts on the browse tab; closing forgets the auctioneer", "[auction]") {
    AuctionHouse house;
    house.opened(5);
    house.selectTab(2);
    house.closed();
    CHECK_FALSE(house.isOpen());
    CHECK(house.auctioneer() == 0);
    house.opened(6);
    CHECK(house.activeTab() == 0);
}

TEST_CASE("An item's arrival redraws only the open lists waiting on it", "[auction]") {
    AuctionHouse house;
    house.opened(5);
    house.listRequested(AuctionList::Owned);
    house.listReceived(listOf({7, 8}), std::nullopt);
    house.listReceived(listOf({8}), AuctionList::Bids);
    CHECK(changedLists(house.itemInfoArrived(8)) == std::vector{AuctionList::Owned, AuctionList::Bids});
    CHECK(changedLists(house.itemInfoArrived(9)).empty());
    house.closed();
    CHECK(changedLists(house.itemInfoArrived(8)).empty());
}

TEST_CASE("A column sort keeps rows that compare equal in their order", "[auction]") {
    AuctionHouse house;
    ListResult r = listOf({3, 1, 2});
    r.auctions[0].stackCount = 1;
    r.auctions[1].stackCount = 5;
    r.auctions[2].stackCount = 1;
    house.listReceived(r, AuctionList::Browse);
    house.reorder(AuctionList::Browse, [](const Entry& a, const Entry& b) { return a.stackCount < b.stackCount; });
    const auto& rows = house.list(AuctionList::Browse).auctions;
    CHECK(rows[0].itemEntry == 3);
    CHECK(rows[1].itemEntry == 2);
    CHECK(rows[2].itemEntry == 1);
}

TEST_CASE("Nothing is sent away from an auctioneer", "[auction]") {
    Fixture f;
    f.service.auctionPlaceBid(1, 100);
    f.openAt(5);
    f.service.closeAuctionHouse();
    f.service.auctionSearch("Linen", 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0);
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("A buyout is a bid at the buyout price", "[auction]") {
    Fixture f;
    f.openAt(5);
    f.service.auctionBuyout(42, 9000);
    CHECK(f.gateway.sent == std::vector<std::string>{"bid 42 9000"});
}

TEST_CASE("A won bid repeats the last search, sort included; a sale does not", "[auction]") {
    Fixture f;
    f.openAt(5);
    f.service.auctionSearch("", 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, {SortKey{2, true}});
    f.gateway.sent.clear();
    f.service.onCommandResult(0, 0);
    f.service.onCommandResult(2, 5);
    CHECK(f.gateway.sent.empty());
    f.service.onCommandResult(2, 0);
    CHECK(f.gateway.sent == std::vector<std::string>{"search 5 '' sort 1"});
}

TEST_CASE("Posting needs an item: a backpack slot or a guid", "[auction]") {
    Fixture f;
    f.openAt(5);
    f.gateway.backpack[3] = {77, 20};
    f.service.auctionSellItem(3, 1, 2, 3);
    f.service.auctionSellItem(4, 1, 2, 3);
    f.service.auctionSellItemByGuid(88, 0, 1, 2, 3);
    f.service.auctionSellItemByGuid(0, 5, 1, 2, 3);
    CHECK(f.gateway.sent == std::vector<std::string>{"sell 77 x20", "sell 88 x1"});
}

TEST_CASE("A list that arrives asks for what its rows need to be drawn", "[auction]") {
    Fixture f;
    f.openAt(5);
    f.service.onList(listOf({11, 12}), AuctionList::Owned);
    CHECK(f.gateway.prepared == std::vector<uint32_t>{11, 12});
    f.service.onItemInfo(11);
    CHECK(f.gateway.prepared.size() == 2);
}

TEST_CASE("The next character is at no auctioneer and has none of the last one's lists", "[auction]") {
    Fixture f;
    f.openAt(5);
    f.service.auctionSearch("Linen", 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0);
    f.service.onList(listOf({11}), std::nullopt);
    f.gateway.sent.clear();

    f.service.beginCharacter();

    CHECK_FALSE(f.service.isAuctionHouseOpen());
    CHECK(f.service.getAuctionBrowseResults().auctions.empty());
    CHECK(f.service.getAuctionSearchDelay() == 0.0f);
    // The last character's search is not repeated for this one.
    f.openAt(6);
    f.service.onCommandResult(2, 0);
    CHECK(f.gateway.sent.empty());
}
