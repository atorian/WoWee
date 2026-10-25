#pragma once

#include "auction/domain/records.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::auction {

/// The auction house as the interface and the addon layer use it.
class IAuctionHouse {
public:
    virtual ~IAuctionHouse() = default;

    [[nodiscard]] virtual bool isAuctionHouseOpen() const = 0;
    [[nodiscard]] virtual uint64_t getAuctioneerGuid() const = 0;
    [[nodiscard]] virtual const ListResult& getAuctionBrowseResults() const = 0;
    [[nodiscard]] virtual const ListResult& getAuctionOwnerResults() const = 0;
    [[nodiscard]] virtual const ListResult& getAuctionBidderResults() const = 0;
    [[nodiscard]] virtual int getAuctionActiveTab() const = 0;
    virtual void setAuctionActiveTab(int tab) = 0;
    /// Seconds until the next search may be sent.
    [[nodiscard]] virtual float getAuctionSearchDelay() const = 0;

    virtual void closeAuctionHouse() = 0;
    virtual void auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                               uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                               uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset = 0,
                               const std::vector<SortKey>& sort = {}) = 0;
    /// Posts the item in a backpack slot.
    virtual void auctionSellItem(int backpackIndex, uint32_t bid, uint32_t buyout, uint32_t duration) = 0;
    /// Posts an item by its server guid, so items in any bag can be sold.
    virtual void auctionSellItemByGuid(uint64_t itemGuid, uint32_t stackCount, uint32_t bid,
                                       uint32_t buyout, uint32_t duration) = 0;
    virtual void auctionPlaceBid(uint32_t auctionId, uint32_t amount) = 0;
    virtual void auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice) = 0;
    virtual void auctionCancelItem(uint32_t auctionId) = 0;
    virtual void auctionListOwnerItems(uint32_t offset = 0) = 0;
    virtual void auctionListBidderItems(uint32_t offset = 0) = 0;
    /// The panel's column sort: the client orders what the server sent.
    virtual void reorderAuctionList(AuctionList list,
                                    const std::function<bool(const Entry&, const Entry&)>& less) = 0;
};

/// `house`, or the auction house of a client with no game: never open, and
/// every request dropped - for a reader that would otherwise test for it at
/// every call.
[[nodiscard]] IAuctionHouse& orNoAuctionHouse(IAuctionHouse* house);

/// What the protocol layer and the client's clock tell the auction house.
class IAuctionHouseFeed {
public:
    virtual ~IAuctionHouseFeed() = default;

    virtual void onHello(uint64_t auctioneerGuid) = 0;
    virtual void onList(ListResult result, std::optional<AuctionList> list) = 0;
    virtual void onCommandResult(uint32_t action, uint32_t error) = 0;
    virtual void onTimePassed(float seconds) = 0;
    virtual void onItemInfo(uint32_t itemEntry) = 0;
    /// A character is logging in: none of the last one's auction house is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::auction
