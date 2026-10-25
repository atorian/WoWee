#include "auction/services/auction_service.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"

namespace wowee::auction {

namespace {

/// The auction house of a client with no game: never open, and every request
/// dropped.
class NoAuctionHouse final : public IAuctionHouse {
public:
    bool isAuctionHouseOpen() const override { return false; }
    uint64_t getAuctioneerGuid() const override { return 0; }
    const ListResult& getAuctionBrowseResults() const override { return empty_; }
    const ListResult& getAuctionOwnerResults() const override { return empty_; }
    const ListResult& getAuctionBidderResults() const override { return empty_; }
    int getAuctionActiveTab() const override { return 0; }
    void setAuctionActiveTab(int) override {}
    float getAuctionSearchDelay() const override { return 0.0f; }
    void closeAuctionHouse() override {}
    void auctionSearch(const std::string&, uint8_t, uint8_t, uint32_t, uint32_t, uint32_t,
                       uint32_t, uint8_t, uint32_t, const std::vector<SortKey>&) override {}
    void auctionSellItem(int, uint32_t, uint32_t, uint32_t) override {}
    void auctionSellItemByGuid(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void auctionPlaceBid(uint32_t, uint32_t) override {}
    void auctionBuyout(uint32_t, uint32_t) override {}
    void auctionCancelItem(uint32_t) override {}
    void auctionListOwnerItems(uint32_t) override {}
    void auctionListBidderItems(uint32_t) override {}
    void reorderAuctionList(AuctionList, const std::function<bool(const Entry&, const Entry&)>&) override {}

private:
    ListResult empty_;
};

NoAuctionHouse kNoAuctionHouse;

} // namespace

IAuctionHouse& orNoAuctionHouse(IAuctionHouse* house) {
    return house ? *house : kNoAuctionHouse;
}

AuctionService::AuctionService(IAuctionGateway& gateway, IAuctionWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

void AuctionService::closeAuctionHouse() {
    apply(house_.closed());
}

void AuctionService::search(const SearchQuery& query) {
    if (!ready()) return;
    gateway_.sendSearch(house_.auctioneer(), query);
    house_.searchSent(query);
}

void AuctionService::auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                                   uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                                   uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset,
                                   const std::vector<SortKey>& sort) {
    search(SearchQuery{.name = name, .levelMin = levelMin, .levelMax = levelMax, .quality = quality,
                       .itemClass = itemClass, .itemSubClass = itemSubClass,
                       .invTypeMask = invTypeMask, .usableOnly = usableOnly, .offset = offset, .sort = sort});
}

void AuctionService::auctionSellItem(int backpackIndex, uint32_t bid, uint32_t buyout, uint32_t duration) {
    if (!ready()) return;
    const auto item = world_.backpackItem(backpackIndex);
    if (!item) return;
    gateway_.sendSell(house_.auctioneer(), item->guid, item->stackCount, bid, buyout, duration);
}

void AuctionService::auctionSellItemByGuid(uint64_t itemGuid, uint32_t stackCount, uint32_t bid,
                                           uint32_t buyout, uint32_t duration) {
    if (!ready()) return;
    if (itemGuid == 0) {
        LOG_ERROR("auctionSellItemByGuid: refusing to post with a null item GUID");
        return;
    }
    gateway_.sendSell(house_.auctioneer(), itemGuid, stackCount == 0 ? 1 : stackCount, bid, buyout, duration);
}

void AuctionService::auctionPlaceBid(uint32_t auctionId, uint32_t amount) {
    if (ready()) gateway_.sendBid(house_.auctioneer(), auctionId, amount);
}

void AuctionService::auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice) {
    if (ready()) gateway_.sendBid(house_.auctioneer(), auctionId, buyoutPrice);
}

void AuctionService::auctionCancelItem(uint32_t auctionId) {
    if (ready()) gateway_.sendCancel(house_.auctioneer(), auctionId);
}

void AuctionService::auctionListOwnerItems(uint32_t offset) {
    if (!ready()) return;
    house_.listRequested(AuctionList::Owned);
    gateway_.sendListOwned(house_.auctioneer(), offset);
}

void AuctionService::auctionListBidderItems(uint32_t offset) {
    if (!ready()) return;
    house_.listRequested(AuctionList::Bids);
    gateway_.sendListBids(house_.auctioneer(), offset);
}

void AuctionService::onList(ListResult result, std::optional<AuctionList> list) {
    const auto changes = house_.listReceived(std::move(result), list);
    apply(changes);
    for (const auto& change : changes) {
        if (const auto* changed = std::get_if<ListChanged>(&change))
            for (const auto& entry : house_.list(changed->list).auctions) world_.prepareToShow(entry);
    }
}

void AuctionService::onCommandResult(uint32_t action, uint32_t error) {
    apply(house_.commandResult(action, error));
    // After a bid or a buyout the list is asked for again, so it shows the
    // change - an empty-name browse of everything included.
    constexpr uint32_t kBid = 2;
    if (error == 0 && action == kBid) {
        if (const SearchQuery* last = house_.lastSearch()) search(*last);
    }
}

void AuctionService::apply(const std::vector<AuctionChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

} // namespace wowee::auction
