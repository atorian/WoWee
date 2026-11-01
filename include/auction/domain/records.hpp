#pragma once

#include "auction/domain/sort_key.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::auction {

/// One auction, as a list result describes it.
struct Entry {
    uint32_t auctionId = 0;
    uint32_t itemEntry = 0;
    uint32_t stackCount = 1;
    uint32_t enchantId = 0;
    uint32_t randomPropertyId = 0;
    uint32_t suffixFactor = 0;
    uint64_t ownerGuid = 0;
    uint32_t startBid = 0;
    uint32_t minBidIncrement = 0;
    uint32_t buyoutPrice = 0;
    uint32_t timeLeftMs = 0;
    uint64_t bidderGuid = 0;
    uint32_t currentBid = 0;
};

/// A page of auctions. `searchDelay` is the server's wait before the next
/// search in milliseconds; zero when the expansion does not send it.
struct ListResult {
    std::vector<Entry> auctions;
    uint32_t totalCount = 0;
    uint32_t searchDelay = 0;
};

/// What the player searched for. 0xFFFFFFFF means "any" for the three filters.
struct SearchQuery {
    std::string name;
    uint8_t levelMin = 0;
    uint8_t levelMax = 0;
    uint32_t quality = 0xFFFFFFFF;
    uint32_t itemClass = 0xFFFFFFFF;
    uint32_t itemSubClass = 0xFFFFFFFF;
    uint32_t invTypeMask = 0;
    uint8_t usableOnly = 0;
    uint32_t offset = 0;
    /// Kept so the search repeated after a purchase asks for the same order;
    /// dropping it would reorder the list under the player as they buy.
    std::vector<SortKey> sort;
};

/// The three lists the auction house panel shows.
enum class AuctionList { Browse, Owned, Bids };

} // namespace wowee::auction
