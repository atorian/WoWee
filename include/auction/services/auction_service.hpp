#pragma once

#include "auction/domain/auction_house.hpp"
#include "auction/services/auction_house.hpp"
#include "auction/services/ports.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::auction {

class AuctionService final : public IAuctionHouse, public IAuctionHouseFeed {
public:
    AuctionService(IAuctionGateway& gateway, IAuctionWorld& world, core::EventBus& events);

    // IAuctionHouse
    [[nodiscard]] bool isAuctionHouseOpen() const override { return house_.isOpen(); }
    [[nodiscard]] uint64_t getAuctioneerGuid() const override { return house_.auctioneer(); }
    [[nodiscard]] const ListResult& getAuctionBrowseResults() const override { return house_.list(AuctionList::Browse); }
    [[nodiscard]] const ListResult& getAuctionOwnerResults() const override { return house_.list(AuctionList::Owned); }
    [[nodiscard]] const ListResult& getAuctionBidderResults() const override { return house_.list(AuctionList::Bids); }
    [[nodiscard]] int getAuctionActiveTab() const override { return house_.activeTab(); }
    void setAuctionActiveTab(int tab) override { house_.selectTab(tab); }
    [[nodiscard]] float getAuctionSearchDelay() const override { return house_.searchDelay(); }
    void closeAuctionHouse() override;
    void auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                       uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                       uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset = 0,
                       const std::vector<SortKey>& sort = {}) override;
    void auctionSellItem(int backpackIndex, uint32_t bid, uint32_t buyout, uint32_t duration) override;
    void auctionSellItemByGuid(uint64_t itemGuid, uint32_t stackCount, uint32_t bid,
                               uint32_t buyout, uint32_t duration) override;
    void auctionPlaceBid(uint32_t auctionId, uint32_t amount) override;
    void auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice) override;
    void auctionCancelItem(uint32_t auctionId) override;
    void auctionListOwnerItems(uint32_t offset = 0) override;
    void auctionListBidderItems(uint32_t offset = 0) override;
    void reorderAuctionList(AuctionList list,
                            const std::function<bool(const Entry&, const Entry&)>& less) override {
        house_.reorder(list, less);
    }

    // IAuctionHouseFeed
    void onHello(uint64_t auctioneerGuid) override { apply(house_.opened(auctioneerGuid)); }
    void onList(ListResult result, std::optional<AuctionList> list) override;
    void onCommandResult(uint32_t action, uint32_t error) override;
    void onTimePassed(float seconds) override { house_.elapse(seconds); }
    void onItemInfo(uint32_t itemEntry) override { apply(house_.itemInfoArrived(itemEntry)); }
    void beginCharacter() override { house_.beginCharacter(); }

private:
    /// At an auctioneer, in the world, with a connection.
    [[nodiscard]] bool ready() const { return gateway_.canSend() && house_.auctioneer() != 0; }
    void search(const SearchQuery& query);
    void apply(const std::vector<AuctionChange>& changes);

    IAuctionGateway& gateway_;
    IAuctionWorld& world_;
    core::EventBus& events_;
    AuctionHouse house_;
};

} // namespace wowee::auction
