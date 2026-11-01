#pragma once

#include "auction/domain/records.hpp"

#include <cstdint>
#include <optional>

namespace wowee::auction {

/// Requests to the auctioneer. Implemented by the protocol layer.
class IAuctionGateway {
public:
    virtual ~IAuctionGateway() = default;

    [[nodiscard]] virtual bool canSend() const = 0;
    virtual void sendSearch(uint64_t auctioneer, const SearchQuery& query) = 0;
    virtual void sendSell(uint64_t auctioneer, uint64_t itemGuid, uint32_t stackCount,
                          uint32_t bid, uint32_t buyout, uint32_t duration) = 0;
    /// A bid; a buyout is a bid at the buyout price.
    virtual void sendBid(uint64_t auctioneer, uint32_t auctionId, uint32_t amount) = 0;
    virtual void sendCancel(uint64_t auctioneer, uint32_t auctionId) = 0;
    virtual void sendListOwned(uint64_t auctioneer, uint32_t offset) = 0;
    virtual void sendListBids(uint64_t auctioneer, uint32_t offset) = 0;
};

/// What the auction house needs from the rest of the game.
class IAuctionWorld {
public:
    virtual ~IAuctionWorld() = default;

    struct BackpackItem { uint64_t guid = 0; uint32_t stackCount = 1; };
    /// The item in a backpack slot, or nothing when the slot is empty or its
    /// server guid is not known.
    [[nodiscard]] virtual std::optional<BackpackItem> backpackItem(int index) const = 0;
    /// Asks for what the panel will draw for a listed auction: the item's
    /// template and the seller's name.
    virtual void prepareToShow(const Entry& entry) = 0;
};

} // namespace wowee::auction
